package cn.sdrstudio.app;
import java.nio.*;
import java.io.IOException;
import org.junit.Test;
import static org.junit.Assert.*;

public class WfmAudioBufferTest {
    private static byte[] packet(int gen,int seq,int samples){
        ByteBuffer b=ByteBuffer.allocate(32+samples*2).order(ByteOrder.LITTLE_ENDIAN);
        b.putInt(0x314d4657).putInt(gen).putInt(seq).putInt(106200000).putInt(1000);
        b.putShort((short)samples).putShort((short)(samples==0?0:1)).putInt(0).putInt(0x12345678);
        for(int n=0;n<samples;n++)b.putShort((short)(n-256));return b.array();
    }
    private static WfmAudioBuffer.Frame frame(int gen,int seq,long now) throws Exception {
        byte[] b=packet(gen,seq,512);return WfmAudioBuffer.decode(b,b.length,now);
    }
    @Test public void wireFormatAndBadLengths() throws Exception {
        byte[] b=packet(17,8,512);WfmAudioBuffer.Frame f=WfmAudioBuffer.decode(b,b.length,10);
        assertEquals(106200000,f.frequency());assertEquals(17,f.generation());assertEquals(0x12345678,f.boot());assertEquals(-256,f.pcm()[0]);assertEquals(255,f.pcm()[511]);
        assertThrows(IOException.class,()->WfmAudioBuffer.decode(b,1055,0));
        b[0]=0;assertThrows(IOException.class,()->WfmAudioBuffer.decode(b,b.length,0));
        byte[] invalid=packet(1,1,513);assertThrows(IOException.class,()->WfmAudioBuffer.decode(invalid,invalid.length,0));
    }
    @Test public void jitterPrimingAndBoundedLatency() throws Exception {
        WfmAudioBuffer q=new WfmAudioBuffer();q.offer(frame(1,1,0));q.offer(frame(1,2,10));assertNull(q.take(10,true));
        for(int n=3;n<=6;n++)q.offer(frame(1,n,20));assertEquals(1,q.take(20,true).sequence());
        for(int n=7;n<=12;n++)q.offer(frame(1,n,30));assertTrue(q.overruns>0);
        assertNull(q.take(151,false));assertTrue(q.overruns>=8);
    }
    @Test public void modeGenerationLossAndDuplicates() throws Exception {
        WfmAudioBuffer q=new WfmAudioBuffer();q.offer(frame(1,1,0));long revision=q.revision();
        q.offer(frame(1,3,10));assertEquals(1,q.lost);assertTrue(q.revision()>revision);
        q.offer(frame(1,2,20));q.offer(frame(1,3,20));assertEquals(2,q.packets);
        q.offer(frame(2,4,20));q.offer(frame(1,9,20));assertEquals(2,q.take(20,false).generation());
        byte[] idle=packet(3,5,0);q.offer(WfmAudioBuffer.decode(idle,idle.length,30));assertNull(q.take(30,false));
    }
}
