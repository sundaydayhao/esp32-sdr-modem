package cn.sdrstudio.app;

import java.io.IOException;
import java.nio.*;
import java.util.ArrayDeque;

/** WFM1 wire decoder and bounded jitter queue, shared by production and host checks. */
final class WfmAudioBuffer {
    static final int RATE=48000,SAMPLES=512,HEADER=32,PACKET=1056;
    record Frame(int generation,int sequence,long frequency,int time,int boot,short[] pcm,long arrived) {}
    static Frame decode(byte[] bytes,int length,long now) throws IOException {
        if(length<HEADER||length>PACKET)throw new IOException("音频包长度无效");
        ByteBuffer b=ByteBuffer.wrap(bytes,0,length).order(ByteOrder.LITTLE_ENDIAN);
        if(b.getInt()!=0x314d4657)throw new IOException("音频协议不匹配");
        int generation=b.getInt(),sequence=b.getInt();long hz=Integer.toUnsignedLong(b.getInt());
        int time=b.getInt(),count=Short.toUnsignedInt(b.getShort()),flags=Short.toUnsignedInt(b.getShort());
        b.getInt();int boot=b.getInt();
        if(count!=0&&count!=SAMPLES||length!=HEADER+2*count||flags!=(count==0?0:1))throw new IOException("音频格式无效");
        short[] pcm=new short[count];for(int n=0;n<count;n++)pcm[n]=b.getShort();
        return new Frame(generation,sequence,hz,time,boot,pcm,now);
    }
    private final ArrayDeque<Frame> queue=new ArrayDeque<>();
    private boolean initialized,haveSequence;
    private int boot,generation,lastSequence;
    private long frequency;
    long resets,packets,lost,overruns;
    synchronized void clear(){queue.clear();resets++;notifyAll();}
    synchronized void offer(Frame f){
        if(initialized&&f.boot==boot&&(f.generation-generation)<0)return;
        if(!initialized||f.boot!=boot||f.generation!=generation||f.frequency!=frequency){
            clear();haveSequence=false;initialized=true;boot=f.boot;generation=f.generation;frequency=f.frequency;
        }
        if(f.pcm.length==0){if(!queue.isEmpty())clear();return;}
        if(haveSequence){int delta=f.sequence-lastSequence;if(delta<=0)return;
            if(delta!=1){lost+=delta-1;clear();}}
        lastSequence=f.sequence;haveSequence=true;packets++;
        if(queue.size()>=8){overruns+=queue.size();clear();}
        queue.addLast(f);notifyAll();
    }
    synchronized Frame take(long now,boolean priming){
        while(!queue.isEmpty()&&now-queue.peekFirst().arrived>120){queue.removeFirst();overruns++;resets++;}
        return queue.size()<(priming?6:1)?null:queue.removeFirst();
    }
    synchronized long revision(){return resets;}
    synchronized String stats(){return "音频包 "+packets+" · 网络丢包 "+lost+" · 缓冲丢弃 "+overruns;}
}
