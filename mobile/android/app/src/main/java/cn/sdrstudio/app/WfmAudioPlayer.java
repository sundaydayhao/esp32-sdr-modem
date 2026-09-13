package cn.sdrstudio.app;

import android.content.*;
import android.media.*;
import android.os.*;
import android.util.Log;
import java.net.*;
import java.nio.charset.StandardCharsets;

/** Socket receiver and AudioTrack writer are independent of HTTP/UI and each other. */
final class WfmAudioPlayer {
    interface Listener {void status(String text);void interrupted();}
    private final DeviceApi api;
    private final Listener listener;
    private final Handler main=new Handler(Looper.getMainLooper());
    private final AudioManager manager;
    private final AudioAttributes attributes=new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build();
    private final AudioFocusRequest focus;
    private volatile Session current;
    private final Context context;
    private final BroadcastReceiver noisy=new BroadcastReceiver(){@Override public void onReceive(Context c,Intent i){stop();listener.interrupted();}};
    private boolean registered;
    WfmAudioPlayer(Context context,DeviceApi api,Listener listener){
        this.context=context;this.api=api;this.listener=listener;manager=(AudioManager)context.getSystemService(Context.AUDIO_SERVICE);
        focus=new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN).setAudioAttributes(attributes)
            .setOnAudioFocusChangeListener(change->{if(change<0){stop();listener.interrupted();}},main).build();
    }
    boolean running(){return current!=null;}
    void start(){
        if(current!=null)return;
        if(manager.requestAudioFocus(focus)!=AudioManager.AUDIOFOCUS_REQUEST_GRANTED){listener.status("手机音频被其他应用占用");listener.interrupted();return;}
        if(Build.VERSION.SDK_INT>=33)context.registerReceiver(noisy,new IntentFilter(AudioManager.ACTION_AUDIO_BECOMING_NOISY),Context.RECEIVER_NOT_EXPORTED);
        else context.registerReceiver(noisy,new IntentFilter(AudioManager.ACTION_AUDIO_BECOMING_NOISY));registered=true;
        Session s=new Session();current=s;
        new Thread(()->receive(s),"wfm-network").start();new Thread(()->play(s),"wfm-output").start();
    }
    void stop(){
        Session s=current;current=null;if(s!=null){s.alive=false;if(s.socket!=null)s.socket.close();s.buffer.clear();}
        manager.abandonAudioFocusRequest(focus);if(registered){context.unregisterReceiver(noisy);registered=false;}
    }
    private static final class Session {
        volatile boolean alive=true;
        volatile DatagramSocket socket;
        volatile String input="正在连接音频…";
        final WfmAudioBuffer buffer=new WfmAudioBuffer();
    }
    private void report(Session s,String text){main.post(()->{if(current==s)listener.status(text);});}
    private static void send(DatagramSocket socket,String text) throws Exception {
        byte[] bytes=text.getBytes(StandardCharsets.US_ASCII);socket.send(new DatagramPacket(bytes,bytes.length));
    }
    private void receive(Session s){
        while(s.alive){
            try(DatagramSocket socket=api.openAudioSocket()){
                s.socket=socket;if(!s.alive)break;
                byte[] bytes=new byte[WfmAudioBuffer.PACKET+1];DatagramPacket packet=new DatagramPacket(bytes,bytes.length);
                long hello=0,lastPacket=SystemClock.elapsedRealtime();
                while(s.alive){
                    long now=SystemClock.elapsedRealtime();
                    if(now-hello>=900){send(socket,"WFM-LISTEN 1\n");hello=now;}
                    try{
                        packet.setLength(bytes.length);socket.receive(packet);now=SystemClock.elapsedRealtime();
                        WfmAudioBuffer.Frame frame=WfmAudioBuffer.decode(bytes,packet.getLength(),now);
                        s.buffer.offer(frame);lastPacket=now;s.input=frame.pcm().length==0?"等待实时 WFM 音频":"手机正在播放 · "+String.format(java.util.Locale.US,"%.3f MHz",frame.frequency()/1e6);
                    }catch(SocketTimeoutException ignored){}
                    if(SystemClock.elapsedRealtime()-lastPacket>3000)throw new java.io.IOException("音频连接中断");
                }
            }catch(Exception e){if(s.alive){s.input="音频断开，正在重连…";s.buffer.clear();try{Thread.sleep(700);}catch(InterruptedException ignored){}}}
            finally{s.socket=null;}
        }
    }
    private void play(Session s){
        android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_AUDIO);
        AudioTrack track=null;long revision=-1,lastData=0,lastReport=0,written=0;boolean priming=true;int prefilled=0,underruns=0;
        try{
            int min=AudioTrack.getMinBufferSize(WfmAudioBuffer.RATE,AudioFormat.CHANNEL_OUT_MONO,AudioFormat.ENCODING_PCM_16BIT);
            if(min<=0)throw new IllegalStateException("手机不支持 48 kHz PCM");
            track=new AudioTrack.Builder().setAudioAttributes(attributes).setAudioFormat(new AudioFormat.Builder()
                .setSampleRate(WfmAudioBuffer.RATE).setChannelMask(AudioFormat.CHANNEL_OUT_MONO).setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
                .setTransferMode(AudioTrack.MODE_STREAM).setBufferSizeInBytes(Math.max(min,8192)).build();
            if(track.getState()!=AudioTrack.STATE_INITIALIZED)throw new IllegalStateException("手机音频输出初始化失败");
            while(s.alive){
                long now=SystemClock.elapsedRealtime(),next=s.buffer.revision();
                int under=track.getUnderrunCount();
                if(revision!=next||under>underruns){track.pause();track.flush();priming=true;prefilled=0;revision=next;}
                underruns=under;
                WfmAudioBuffer.Frame f=s.buffer.take(now,priming&&prefilled==0);
                if(f!=null){
                    if(s.buffer.revision()!=revision)continue;
                    int offset=0;while(s.alive&&offset<f.pcm().length){int n=track.write(f.pcm(),offset,f.pcm().length-offset,AudioTrack.WRITE_BLOCKING);if(n<=0)throw new IllegalStateException("手机音频写入失败："+n);offset+=n;written+=n;}
                    if(priming&&++prefilled>=6){track.play();priming=false;}
                    lastData=now;
                }else{
                    if(!priming&&now-lastData>150){track.pause();track.flush();priming=true;prefilled=0;}
                    Thread.sleep(4);
                }
                if(now-lastReport>=1000){
                    String message=(priming&&s.input.startsWith("手机正在播放")?"正在缓冲 WFM 音频…":s.input)+"\n"+s.buffer.stats();report(s,message);
                    Log.i("WfmAudio","samples="+written+" head="+Integer.toUnsignedLong(track.getPlaybackHeadPosition())+" underruns="+track.getUnderrunCount()+" "+s.buffer.stats());lastReport=now;
                }
            }
        }catch(Exception e){report(s,"手机播放失败："+e.getMessage());main.post(()->{if(current==s){stop();listener.interrupted();}});}
        finally{if(track!=null){track.pause();track.flush();track.release();}}
    }
}
