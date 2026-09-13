package cn.sdrstudio.app;
import android.content.Context;
import android.graphics.*;
import android.view.View;
import org.json.*;
import java.util.Arrays;

final class SpectrumView extends View {
    private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path trace=new Path();
    private Bitmap waterfall;
    private int[] bins=new int[512];
    private long sequence=-1,generation=-1;
    private float floor=-1;
    private boolean live,replay,scan;
    private double left=0,right=0;
    SpectrumView(Context c){super(c);setContentDescription("实时频谱与瀑布图");}
    static int color(int level) {
        int[][] colors={{2,5,14},{8,15,48},{14,38,104},{16,90,178},{0,175,215},{110,215,130},{250,225,65},{255,130,30},{255,250,235}};
        int part=Math.min(7,level*8/255),step=level*8-part*255;int[] a=colors[part],b=colors[part+1];
        return Color.rgb(a[0]+(b[0]-a[0])*step/255,a[1]+(b[1]-a[1])*step/255,a[2]+(b[2]-a[2])*step/255);
    }
    void stale(){live=false;invalidate();}
    void update(JSONObject s) {
        JSONObject c=s.optJSONObject("config");if(c==null)return;
        live=s.optBoolean("streaming");replay=s.optBoolean("replay_source");scan=c.optInt("mode")==7;
        double center=s.optDouble("active_frequency_hz"),rate=s.optDouble("sample_rate_hz");
        left=(scan?c.optDouble("scan_start_hz"):center-rate/2)/1e6;right=(scan?c.optDouble("scan_stop_hz"):center+rate/2)/1e6;
        long epoch=s.optLong("view_generation",s.optLong("generation"));
        if(epoch!=generation){generation=epoch;floor=-1;sequence=-1;if(waterfall!=null)waterfall.eraseColor(Color.rgb(2,5,14));}
        long seq=s.optLong("sequence");JSONArray data=s.optJSONArray(scan?"scan":"fft");
        if(data!=null&&data.length()==512&&seq!=sequence&&(live||replay)) {
            for(int n=0;n<512;n++)bins[n]=Math.max(0,Math.min(255,data.optInt(n)));
            int[] sorted=bins.clone();Arrays.sort(sorted);float target=Math.max(0,sorted[102]-6);floor=floor<0?target:floor+(target-floor)/16;
            if(waterfall==null)waterfall=Bitmap.createBitmap(512,160,Bitmap.Config.RGB_565);
            int[] rows=new int[512*159];waterfall.getPixels(rows,0,512,0,0,512,159);waterfall.setPixels(rows,0,512,0,1,512,159);
            int[] row=new int[512];for(int n=0;n<512;n++)row[n]=color(Math.max(0,Math.min(255,(int)((bins[n]-floor)*255/73))));
            waterfall.setPixels(row,0,512,0,0,512,1);sequence=seq;
        }
        invalidate();
    }
    @Override protected void onDraw(Canvas c) {
        super.onDraw(c);int w=getWidth(),h=getHeight();c.drawColor(Color.rgb(8,15,25));
        paint.setColor(0xff26364a);paint.setStrokeWidth(1);
        for(int n=1;n<5;n++){c.drawLine(w*n/5f,0,w*n/5f,h-30,paint);c.drawLine(0,(h-30)*n/5f,w,(h-30)*n/5f,paint);}
        if(sequence>=0) {
            if(waterfall!=null)c.drawBitmap(waterfall,null,new Rect(0,h/3,w,h-32),null);
            trace.reset();for(int n=0;n<512;n++){float x=n*(w-1)/511f,y=h/3f-4-bins[n]*(h/3f-10)/255;if(n==0)trace.moveTo(x,y);else trace.lineTo(x,y);}
            paint.setColor(0xff5de0c0);paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(2);c.drawPath(trace,paint);paint.setStyle(Paint.Style.FILL);
        }
        paint.setTextSize(12*getResources().getDisplayMetrics().scaledDensity);paint.setColor(0xff90a3bc);
        c.drawText(String.format(java.util.Locale.US,"%.3f",left),8,h-8,paint);String end=String.format(java.util.Locale.US,"%.3f MHz",right);c.drawText(end,w-paint.measureText(end)-8,h-8,paint);
        if(sequence<0||!live&&!replay){paint.setColor(0xbd0b111b);c.drawRect(0,0,w,h-32,paint);paint.setColor(0xffecf2fa);String text=sequence<0?"等待真实接收数据":"接收已暂停 / 当前图像为历史数据";c.drawText(text,(w-paint.measureText(text))/2,h/2f,paint);}
    }
}
