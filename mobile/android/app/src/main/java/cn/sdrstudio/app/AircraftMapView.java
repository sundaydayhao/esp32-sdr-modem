package cn.sdrstudio.app;

import android.content.Context;
import android.graphics.*;
import android.net.*;
import android.util.LruCache;
import android.view.*;
import org.json.*;
import java.io.*;
import java.net.*;
import java.util.*;
import java.util.concurrent.*;

/** Native map; WGS84 aircraft use Web Mercator with no GCJ-02 shift. */
final class AircraftMapView extends View {
    interface Selection {void selected(JSONObject aircraft);}
    private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
    private final ExecutorService loader=Executors.newFixedThreadPool(2);
    private final LruCache<String,Bitmap> tiles=new LruCache<>(12*1024*1024){protected int sizeOf(String k,Bitmap b){return b.getByteCount();}};
    private final Set<String> pending=Collections.synchronizedSet(new HashSet<>());
    private final Map<String,Long> failed=new ConcurrentHashMap<>();
    private final File cache;
    private final GestureDetector gestures;
    private final ScaleGestureDetector pinch;
    private final ConnectivityManager connectivity;
    private JSONArray aircraft=new JSONArray(),paths=new JSONArray(),places=new JSONArray();
    private double latitude=22.60,longitude=114.05;
    private int zoom=12;
    private boolean fresh=false,follow=true,closed=false;
    private String selected="",template="https://tile.openstreetmap.org/{z}/{x}/{y}.png";
    private Selection listener;
    private volatile String mapStatus="离线区域底图";
    AircraftMapView(Context context) {
        super(context);setContentDescription("ADS-B 飞机地图，可拖动或双指缩放");
        connectivity=(ConnectivityManager)context.getSystemService(Context.CONNECTIVITY_SERVICE);
        cache=new File(context.getCacheDir(),"map-tiles");cache.mkdirs();
        loader.execute(()->{
            try(InputStream in=context.getAssets().open("sdr_map_region.json")){
                ByteArrayOutputStream bytes=new ByteArrayOutputStream();byte[] buffer=new byte[8192];int count;
                while((count=in.read(buffer))!=-1){if(bytes.size()+count>8*1024*1024)throw new IOException("离线地图超出上限");bytes.write(buffer,0,count);}
                JSONObject region=new JSONObject(new String(bytes.toByteArray(),java.nio.charset.StandardCharsets.UTF_8));
                post(()->{paths=region.optJSONArray("paths");places=region.optJSONArray("places");invalidate();});
            }catch(Exception e){mapStatus="区域底图不可用";postInvalidate();}
        });
        gestures=new GestureDetector(context,new GestureDetector.SimpleOnGestureListener(){
            public boolean onDown(MotionEvent e){return true;}
            public boolean onScroll(MotionEvent a,MotionEvent b,float dx,float dy){double size=256.0*(1<<zoom);longitude=MapProjection.wrap(longitude+dx/size*360);latitude=MapProjection.latitude(MapProjection.y(latitude)+dy/size);follow=false;invalidate();return true;}
            public boolean onDoubleTap(MotionEvent e){changeZoom(1);return true;}
            public boolean onSingleTapUp(MotionEvent e){selectAt(e.getX(),e.getY());return true;}
        });
        pinch=new ScaleGestureDetector(context,new ScaleGestureDetector.SimpleOnScaleGestureListener(){
            private float scale=1;public boolean onScaleBegin(ScaleGestureDetector d){scale=1;return true;}
            public boolean onScale(ScaleGestureDetector d){scale*=d.getScaleFactor();if(scale>1.5){changeZoom(1);scale=1;}if(scale<.67){changeZoom(-1);scale=1;}return true;}
        });
    }
    void selection(Selection callback){listener=callback;}
    void mapTemplate(String value){if(!value.startsWith("https://")||!value.contains("{z}")||!value.contains("{x}")||!value.contains("{y}"))throw new IllegalArgumentException("地图地址须为 HTTPS，包含 {z}/{x}/{y}");template=value;tiles.evictAll();failed.clear();invalidate();}
    void close(){closed=true;loader.shutdownNow();}
    void stale(){fresh=false;invalidate();}
    void changeZoom(int delta){zoom=Math.max(2,Math.min(18,zoom+delta));follow=false;invalidate();}
    int zoomLevel(){return zoom;}
    void fit(){follow=true;fitAircraft();invalidate();}
    void focus(JSONObject a){if(valid(a)){latitude=a.optDouble("latitude");longitude=a.optDouble("longitude");zoom=13;selected=a.optString("icao");follow=false;invalidate();}}
    static boolean valid(JSONObject a){return a!=null&&a.optBoolean("has_position")&&a.optLong("position_age_ms",0)<=60000&&Double.isFinite(a.optDouble("latitude"))&&Math.abs(a.optDouble("latitude"))<=90&&Double.isFinite(a.optDouble("longitude"))&&Math.abs(a.optDouble("longitude"))<=180;}
    void update(JSONArray values){aircraft=values==null?new JSONArray():values;fresh=true;if(follow)fitAircraft();invalidate();}
    private void fitAircraft(){
        boolean first=true;double ref=0,west=0,east=0,north=0,south=0;
        for(int i=0;i<aircraft.length();i++){JSONObject a=aircraft.optJSONObject(i);if(!valid(a))continue;double lat=a.optDouble("latitude"),lon=a.optDouble("longitude");
            if(first){ref=lon;north=south=lat;first=false;}double d=MapProjection.wrap(lon-ref);west=Math.min(west,d);east=Math.max(east,d);north=Math.max(north,lat);south=Math.min(south,lat);}
        if(first)return;latitude=(north+south)/2;longitude=MapProjection.wrap(ref+(west+east)/2);
        double fraction=Math.max((east-west)/360,Math.abs(MapProjection.y(north)-MapProjection.y(south)));zoom=Math.max(2,Math.min(14,(int)Math.floor(Math.log(Math.max(1,Math.min(getWidth(),getHeight()))/(256*Math.max(.00005,fraction)*1.5))/Math.log(2))));
    }
    private float px(double lon){return (float)(getWidth()/2.0+MapProjection.deltaX(lon,longitude)*256*(1<<zoom));}
    private float py(double lat){return (float)(getHeight()/2.0+(MapProjection.y(lat)-MapProjection.y(latitude))*256*(1<<zoom));}
    @Override public boolean onTouchEvent(MotionEvent event){getParent().requestDisallowInterceptTouchEvent(true);pinch.onTouchEvent(event);gestures.onTouchEvent(event);if(event.getActionMasked()==MotionEvent.ACTION_UP||event.getActionMasked()==MotionEvent.ACTION_CANCEL)getParent().requestDisallowInterceptTouchEvent(false);return true;}
    private void selectAt(float x,float y){JSONObject found=null;double distance=40*getResources().getDisplayMetrics().density;
        for(int i=0;i<aircraft.length();i++){JSONObject a=aircraft.optJSONObject(i);if(!valid(a))continue;double d=Math.hypot(x-px(a.optDouble("longitude")),y-py(a.optDouble("latitude")));if(d<distance){found=a;distance=d;}}
        if(found!=null){selected=found.optString("icao");if(listener!=null)listener.selected(found);invalidate();}
    }
    private void offline(Canvas canvas){
        if(paths==null)return;Path path=new Path();
        for(int n=0;n<paths.length();n++){
            JSONObject f=paths.optJSONObject(n);JSONArray points=f.optJSONArray("points");if(points==null)continue;int kind=f.optInt("kind");if(zoom<10&&kind>=4||zoom<12&&kind>=5)continue;
            // Native region is small and bounded; skip feature boxes outside the viewport.
            float minX=Float.MAX_VALUE,minY=Float.MAX_VALUE,maxX=-Float.MAX_VALUE,maxY=-Float.MAX_VALUE;path.reset();
            for(int i=0;i<points.length();i++){JSONArray p=points.optJSONArray(i);float x=px(p.optDouble(0)/100000),y=py(p.optDouble(1)/100000);minX=Math.min(minX,x);maxX=Math.max(maxX,x);minY=Math.min(minY,y);maxY=Math.max(maxY,y);if(i==0)path.moveTo(x,y);else path.lineTo(x,y);}
            if(maxX<0||maxY<0||minX>getWidth()||minY>getHeight())continue;
            int[] colors={0xff558ba0,0xff305974,0xffd2dbe5,0xff9d8450,0xff586879,0xff354556};paint.setColor(colors[Math.min(kind,5)]);paint.setStrokeWidth(kind==2?3:kind==3?2:1);paint.setStyle(Paint.Style.STROKE);canvas.drawPath(path,paint);
        }paint.setStyle(Paint.Style.FILL);
        java.util.ArrayList<RectF> labels=new java.util.ArrayList<>();float density=getResources().getDisplayMetrics().density;
        if(places!=null)for(int i=0;i<places.length();i++){JSONObject p=places.optJSONObject(i);if(zoom<11&&p.optInt("rank")>0||zoom<13&&p.optInt("rank")>1)continue;float x=px(p.optDouble("lon")),y=py(p.optDouble("lat"));if(x<5||x>getWidth()-40||y<20||y>getHeight()-42*density)continue;
            paint.setTextSize(12*getResources().getDisplayMetrics().scaledDensity);String name=p.optString("name");RectF area=new RectF(x-4*density,y-15*density,x+paint.measureText(name)+8*density,y+6*density);boolean overlaps=false;for(RectF prior:labels)if(RectF.intersects(prior,area)){overlaps=true;break;}if(overlaps||area.right>getWidth())continue;labels.add(area);paint.setColor(0xffabbcd0);canvas.drawText(name,x,y,paint);}
    }
    @Override protected void onDraw(Canvas canvas){
        canvas.drawColor(0xff101e2a);offline(canvas);double size=256.0*(1<<zoom);double cx=MapProjection.x(longitude)*size,cy=MapProjection.y(latitude)*size;
        int left=(int)Math.floor((cx-getWidth()/2.0)/256),top=(int)Math.floor((cy-getHeight()/2.0)/256),right=(int)Math.floor((cx+getWidth()/2.0)/256),bottom=(int)Math.floor((cy+getHeight()/2.0)/256);
        int loaded=0,total=0;for(int ty=top;ty<=bottom;ty++)for(int tx=left;tx<=right;tx++){
            if(ty<0||ty>=(1<<zoom))continue;int xx=((tx%(1<<zoom))+(1<<zoom))%(1<<zoom);String key=Integer.toHexString(template.hashCode())+"_"+zoom+"_"+xx+"_"+ty;Bitmap tile=tiles.get(key);total++;
            float x=(float)(tx*256-cx+getWidth()/2.0),y=(float)(ty*256-cy+getHeight()/2.0);
            if(tile!=null){canvas.drawBitmap(tile,x,y,null);loaded++;}else request(key,zoom,xx,ty);
        }
        int positioned=0;for(int i=0;i<aircraft.length();i++){
            JSONObject a=aircraft.optJSONObject(i);if(!valid(a))continue;positioned++;float x=px(a.optDouble("longitude")),y=py(a.optDouble("latitude"));if(x<-20||x>getWidth()+20||y<-20||y>getHeight()+20)continue;
            boolean active=selected.equals(a.optString("icao"));paint.setColor(fresh?(active?0xffffbf55:0xff15b697):0xff8a96a6);float s=(active?16:12)*getResources().getDisplayMetrics().density;
            canvas.save();canvas.translate(x,y);canvas.rotate((float)a.optDouble("heading_degrees",0));Path arrow=new Path();arrow.moveTo(0,-s);arrow.lineTo(s*.6f,s*.8f);arrow.lineTo(0,s*.35f);arrow.lineTo(-s*.6f,s*.8f);arrow.close();canvas.drawPath(arrow,paint);paint.setColor(0xff0b111b);paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(1.5f);canvas.drawPath(arrow,paint);paint.setStyle(Paint.Style.FILL);canvas.restore();
            String label=a.optString("callsign","").trim();if(label.isEmpty())label=a.optString("icao");paint.setTextSize(13*getResources().getDisplayMetrics().scaledDensity);paint.setColor(0xdf0b111b);canvas.drawRect(x+12,y-21,x+18+paint.measureText(label),y+4,paint);paint.setColor(0xffecf2fa);canvas.drawText(label,x+15,y,paint);
        }
        float density=getResources().getDisplayMetrics().density;paint.setColor(0xe00b111b);canvas.drawRect(0,getHeight()-40*density,getWidth(),getHeight(),paint);paint.setTextSize(10*getResources().getDisplayMetrics().scaledDensity);paint.setColor(0xffc2d0e0);
        String source=loaded==total&&total>0?"街道底图":mapStatus;
        String caption=String.format(Locale.US,"%.4f, %.4f · z%d · %s",latitude,longitude,zoom,source);
        while(caption.length()>1&&paint.measureText(caption)>getWidth()-90*density)caption=caption.substring(0,caption.length()-1);
        canvas.drawText(caption,8,getHeight()-24*density,paint);
        canvas.drawText("© OpenStreetMap contributors · ODbL",8,getHeight()-9*density,paint);
        float bar=64*density;double unit=MapProjection.metersPerPixel(latitude,zoom)*bar;String scale=unit>=1000?String.format(Locale.US,"%.1f km",unit/1000):String.format(Locale.US,"%.0f m",unit);float sx=getWidth()-74*density;
        paint.setStrokeWidth(2);canvas.drawLine(sx,getHeight()-19*density,sx+bar,getHeight()-19*density,paint);canvas.drawText(scale,sx,getHeight()-26*density,paint);
        if(positioned==0){paint.setColor(0xcd0b111b);canvas.drawRect(0,0,getWidth(),35*density,paint);paint.setColor(0xffecf2fa);paint.setTextSize(13*getResources().getDisplayMetrics().scaledDensity);canvas.drawText("暂无有效飞机位置 · 等待真实 ADS-B 接收",10,23*density,paint);}
        if(!fresh){paint.setColor(0xffedb568);paint.setTextSize(12*getResources().getDisplayMetrics().scaledDensity);canvas.drawText("设备未连接 / 飞机数据未更新",10,55*density,paint);}
    }
    private void request(String key,int z,int x,int y){
        if(closed||pending.size()>=20||failed.getOrDefault(key,0L)>System.currentTimeMillis()||!pending.add(key))return;
        final String url=template.replace("{z}",""+z).replace("{x}",""+x).replace("{y}",""+y);
        loader.execute(()->{
            try {
                File file=new File(cache,key+".png");Bitmap bitmap=null;
                if(file.exists())bitmap=BitmapFactory.decodeFile(file.getPath());
                if(bitmap!=null){tiles.put(key,bitmap);if(System.currentTimeMillis()-file.lastModified()<7L*86400000)return;}
                Network internet=null;for(Network n:connectivity.getAllNetworks()){NetworkCapabilities c=connectivity.getNetworkCapabilities(n);if(c!=null&&c.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED)&&c.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)){internet=n;break;}}
                if(internet==null)throw new IOException("无互联网，显示离线区域底图");
                HttpURLConnection connection=(HttpURLConnection)internet.openConnection(new URL(url));
                connection.setConnectTimeout(3500);connection.setReadTimeout(5000);connection.setInstanceFollowRedirects(false);connection.setRequestProperty("User-Agent","SDRStudioAndroid/0.1 (cn.sdrstudio.app)");
                try {
                    if(connection.getResponseCode()!=200)throw new IOException("地图暂不可用，显示离线底图");
                    byte[] bytes;try(InputStream in=connection.getInputStream()){ByteArrayOutputStream out=new ByteArrayOutputStream();byte[] block=new byte[8192];int n;while((n=in.read(block))!=-1){if(out.size()+n>1024*1024)throw new IOException("地图瓦片过大");out.write(block,0,n);}bytes=out.toByteArray();}
                    Bitmap next=BitmapFactory.decodeByteArray(bytes,0,bytes.length);if(next==null)throw new IOException("地图格式无效");tiles.put(key,next);
                    File temp=new File(cache,key+".tmp");try(FileOutputStream out=new FileOutputStream(temp)){out.write(bytes);}if(!temp.renameTo(file))temp.delete();
                    trimCache();mapStatus="街道底图 / 部分载入";
                }finally{connection.disconnect();}
            }catch(Exception e){failed.put(key,System.currentTimeMillis()+60000);mapStatus="离线区域底图";}
            finally{pending.remove(key);postInvalidate();}
        });
    }
    private void trimCache(){File[] files=cache.listFiles((d,n)->n.endsWith(".png"));if(files==null)return;long size=0;for(File f:files)size+=f.length();if(size<100L*1024*1024)return;Arrays.sort(files,Comparator.comparingLong(File::lastModified));for(File f:files){if(size<80L*1024*1024)break;long length=f.length();if(f.delete())size-=length;}}
}
