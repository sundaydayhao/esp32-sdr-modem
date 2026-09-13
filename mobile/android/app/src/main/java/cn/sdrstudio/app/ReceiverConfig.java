package cn.sdrstudio.app;
import org.json.*;

/** Wire mode values match sdr_types.h. SD replay is a UI selection, never mode 10 on the wire. */
final class ReceiverConfig {
    static final String[] MODES={"USB 上边带","LSB 下边带","AM 调幅","WFM 广播","NFM 窄带调频","频谱分析","ADS-B 飞机","频段扫描","Sub-GHz 接收","IQ 采集至 SD","SD IQ 重放"};
    static final String[] FRONTENDS={"调谐器","Q 路直采","Blog V4","上变频器"};
    static final int[] REPLAY={5,3,4,2,0,1,8};
    static final String[] REPLAY_LABELS={"频谱","WFM 音频","NFM 音频","AM 音频","USB 音频","LSB 音频","OOK 脉冲"};
    static final String[] BANDS={"433.92 MHz","315.00 MHz","431.00 MHz","490.00 MHz","868.30 MHz"};
    static final long[][] PRESETS={{433920000,433050000,434790000},{315000000,314000000,316000000},
        {431000000,430000000,432000000},{490000000,470000000,510000000},{868300000,868000000,868600000}};
    static boolean audio(int mode){return mode>=0&&mode<=4;}
    static long mhz(String text) {
        try {double n=Double.parseDouble(text.trim());if(!Double.isFinite(n)||n<=0||n>2000)throw new NumberFormatException();
            return Math.round(n*1000000);
        }catch(NumberFormatException e){throw new IllegalArgumentException("频率须在 0–2000 MHz 内");}
    }
    static JSONObject defaults() {
        try{return new JSONObject("{\"enabled\":true,\"decode\":false,\"monitor\":true,\"mode\":3,\"frontend\":0,\"frequency_hz\":106200000,\"converter_hz\":125000000,\"scan_start_hz\":88000000,\"scan_stop_hz\":108000000,\"volume\":35}");}
        catch(JSONException e){throw new AssertionError(e);}
    }
    static JSONObject select(JSONObject old,int selection,int band) throws JSONException {
        if(selection<0||selection>9)throw new IllegalArgumentException("重放通过 SD 接口启动");
        JSONObject c=new JSONObject(old.toString());c.put("mode",selection);c.put("enabled",true);
        c.put("monitor",audio(selection));c.put("decode",selection==0||selection==1);
        if(selection==6){c.put("frequency_hz",1090000000);c.put("frontend",0);}
        if(selection==3){long f=c.optLong("frequency_hz");if(f<88000000||f>108000000)c.put("frequency_hz",106200000);c.put("frontend",0);}
        if(selection==8)applyBand(c,band);
        return c;
    }
    static void applyBand(JSONObject c,int index) throws JSONException {
        if(index<0||index>=PRESETS.length)throw new IllegalArgumentException("频段预设无效");
        long[] b=PRESETS[index];c.put("frequency_hz",b[0]);c.put("scan_start_hz",b[1]);c.put("scan_stop_hz",b[2]);
        c.put("mode",index==3?5:8);c.put("frontend",0);c.put("enabled",true);c.put("decode",false);c.put("monitor",false);
    }
    static boolean busy(JSONObject s){return s.optBoolean("recording")||s.optBoolean("record_saving")||s.optBoolean("replaying");}
}
