package cn.sdrstudio.app;
import android.content.Intent;
import android.graphics.*;
import android.os.Looper;
import android.view.*;
import android.widget.*;
import org.json.*;
import org.junit.*;
import org.junit.runner.RunWith;
import org.robolectric.*;
import org.robolectric.android.controller.ActivityController;
import org.robolectric.annotation.*;
import java.io.*;
import java.lang.reflect.*;
import java.net.*;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.atomic.*;
import static org.junit.Assert.*;

/** Runs production Android views and HTTP client against a labelled host fixture. */
@RunWith(RobolectricTestRunner.class)
@Config(sdk=34,qualifiers="w411dp-h891dp-xxhdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class NativeUiTest {
    private FixtureServer server;private ActivityController<MainActivity> controller;private MainActivity activity;
    private final AtomicReference<JSONObject> current=new AtomicReference<>(ReceiverConfig.defaults());
    private final AtomicInteger writes=new AtomicInteger(),generation=new AtomicInteger(1);
    private String base;
    @Before public void start() throws Exception {
        server=new FixtureServer();base="http://127.0.0.1:"+server.socket.getLocalPort();
        server.handler=exchange->{try {
            String path=exchange.path;JSONObject reply;
            if(path.equals("/api/info"))reply=new JSONObject("{\"product\":\"sdr-studio\",\"api_version\":1,\"source\":\"simulation\"}");
            else if(path.equals("/api/sdr")){
                if(exchange.method.equals("POST")){
                    JSONObject c=new JSONObject(new String(exchange.body,StandardCharsets.UTF_8));
                    if(c.optInt("volume")>100){send(exchange,400,"Volume must be 0..100");return;}
                    current.set(c);writes.incrementAndGet();generation.incrementAndGet();
                }reply=status();
            }else if(path.equals("/api/aircraft"))reply=new JSONObject("{\"aircraft\":[{\"icao\":\"ABC001\",\"callsign\":\"HOST SZ\",\"has_position\":true,\"latitude\":22.57,\"longitude\":114.06,\"altitude_feet\":7800,\"position_age_ms\":100},{\"icao\":\"ABC002\",\"callsign\":\"NO CPR\",\"has_position\":false}]}");
            else if(path.equals("/api/sdr/sd"))reply=status();
            else if(path.equals("/api/modes")){send(exchange,200,"[{\"id\":0,\"name\":\"BPSK31\"},{\"id\":26,\"name\":\"8PSK1200F\"}]");return;}
            else if(path.equals("/api/status"))reply=new JSONObject("{\"boot_id\":\"HOST-TEST-ONLY\",\"content_type\":2,\"mode_id\":26,\"mode\":\"8PSK1200F\",\"tone_hz\":2000,\"role\":\"RX\"}");
            else if(path.equals("/api/config")){send(exchange,202,"{\"queued\":true}");return;}
            else {send(exchange,404,"No page");return;}
            send(exchange,200,reply.toString());
        }catch(Exception e){send(exchange,500,e.toString());}};server.start();
        Intent intent=new Intent(RuntimeEnvironment.getApplication(),MainActivity.class).putExtra("test_endpoint",base);
        controller=Robolectric.buildActivity(MainActivity.class,intent).create().start().resume().visible();activity=controller.get();
        await(()->{try{return (boolean)field("connected");}catch(Exception e){return false;}});
    }
    private JSONObject status() throws Exception {
        JSONObject j=new JSONObject();j.put("config",new JSONObject(current.get().toString()));j.put("boot_id","HOST-TEST-ONLY");j.put("streaming",true);j.put("generation",generation.get());j.put("view_generation",generation.get());j.put("sequence",System.nanoTime());j.put("sample_rate_hz",current.get().optInt("mode")==6?2000000:288000);j.put("active_frequency_hz",current.get().getLong("frequency_hz"));j.put("usb_status","HOST TEST / simulated");j.put("audio_level",12345);j.put("sd_mounted",true);j.put("sd_state",0);j.put("sd_free_mb",29000);j.put("sd_files",new JSONArray("[{\"name\":\"IQ000001.IQ\",\"bytes\":5760000,\"frequency_hz\":433920000}]") );
        JSONArray fft=new JSONArray();for(int n=0;n<512;n++)fft.put((int)(120+70*Math.exp(-Math.pow((n-310)/9.,2))));j.put("fft",fft);j.put("scan",fft);return j;
    }
    private static void send(Exchange e,int code,String body) throws IOException {byte[] data=body.getBytes(StandardCharsets.UTF_8);e.out.write(("HTTP/1.1 "+code+" Result\r\nContent-Type: application/json\r\nContent-Length: "+data.length+"\r\nConnection: close\r\n\r\n").getBytes(StandardCharsets.US_ASCII));e.out.write(data);e.out.flush();}
    private Object field(String name) throws Exception {Field f=MainActivity.class.getDeclaredField(name);f.setAccessible(true);return f.get(activity);}
    private void await(java.util.function.BooleanSupplier check) throws Exception {long end=System.nanoTime()+10000000000L;do{Shadows.shadowOf(Looper.getMainLooper()).idle();if(check.getAsBoolean())return;Thread.sleep(30);}while(System.nanoTime()<end);fail("Timed out waiting for the native UI/HTTP fixture");}
    @After public void close(){if(controller!=null)controller.pause().stop().destroy();if(server!=null)server.stop(0);}
    @Test public void nativeModeChangeAndRadioAudioRoundTrip() throws Exception {
        assertEquals(View.VISIBLE,((View)field("phoneAudioBox")).getVisibility());
        assertFalse(((CheckBox)field("phoneListen")).isChecked());
        Spinner selector=(Spinner)field("modes");assertEquals(11,selector.getCount());assertTrue(((TextView)field("connection")).getText().toString().contains("HOST TEST"));
        selector.setSelection(6);await(()->current.get().optInt("mode")==6);await(()->{try{return !((boolean)field("commandPending"));}catch(Exception e){return false;}});
        assertEquals(1090000000,current.get().getLong("frequency_hz"));assertEquals(View.VISIBLE,((View)field("mapPanel")).getVisibility());
        assertEquals(View.GONE,((View)field("phoneAudioBox")).getVisibility());
        // Force a fresh poll without advancing the entire repeating timer queue.
        ((Runnable)field("poll")).run();await(()->{try{return ((LinearLayout)field("aircraftList")).getChildCount()==2;}catch(Exception e){return false;}});
        screenshot("android-adsb-host.png");
        selector.setSelection(9);await(()->current.get().optInt("mode")==9);await(()->{try{return !((boolean)field("commandPending"));}catch(Exception e){return false;}});assertFalse(current.get().getBoolean("monitor"));
        selector.setSelection(3);await(()->current.get().optInt("mode")==3);await(()->{try{return !((boolean)field("commandPending"));}catch(Exception e){return false;}});assertTrue(current.get().getBoolean("monitor"));
        ((CheckBox)field("monitor")).setChecked(false);await(()->!current.get().optBoolean("monitor"));
        assertTrue(writes.get()>=4);screenshot("android-radio-host.png");
    }
    @Test public void httpErrorsAndOfflineControlsAreVisible() throws Exception {
        DeviceApi api=new DeviceApi(activity,base);JSONObject invalid=ReceiverConfig.defaults();invalid.put("volume",101);
        assertThrows(IOException.class,()->api.post("/api/sdr",invalid));assertThrows(IOException.class,()->api.get("/"));
        server.stop(0);server=null;((Runnable)field("poll")).run();await(()->{try{return !((boolean)field("connected"));}catch(Exception e){return false;}});
        assertFalse(((Spinner)field("modes")).isEnabled());assertTrue(((TextView)field("notice")).getText().toString().contains("连接失败"));
        screenshot("android-offline-host.png");
    }
    @Test public void sdRefreshKeepsReplaySelectionWithoutRetuning() throws Exception {
        Spinner selector=(Spinner)field("modes");int before=writes.get();
        selector.setSelection(10);
        await(()->{try{return (int)field("selection")==10&&!((boolean)field("commandPending"));}catch(Exception e){return false;}});
        ((Runnable)field("poll")).run();
        await(()->{try{return !((boolean)field("polling"));}catch(Exception e){return false;}});
        assertEquals(10,selector.getSelectedItemPosition());
        assertEquals(View.VISIBLE,((View)field("replayBox")).getVisibility());
        assertEquals("IQ000001.IQ",((Spinner)field("sdFile")).getSelectedItem());
        assertEquals(before,writes.get());assertEquals(3,current.get().getInt("mode"));
    }
    @Test public void pollingResumesWhenATimerFiresDuringACommand() throws Exception {
        Field pending=MainActivity.class.getDeclaredField("commandPending");pending.setAccessible(true);
        pending.setBoolean(activity,true);((Runnable)field("poll")).run();
        current.get().put("frequency_hz",101700000);pending.setBoolean(activity,false);
        Shadows.shadowOf(Looper.getMainLooper()).idleFor(java.time.Duration.ofMillis(350));
        await(()->{try{return ((JSONObject)field("config")).optLong("frequency_hz")==101700000;}catch(Exception e){return false;}});
        assertTrue((boolean)field("connected"));
    }
    @Test public void modemPageLoadsActualSelectionAndAcceptsQueuedResponse() throws Exception {
        Method show=MainActivity.class.getDeclaredMethod("showPage",int.class);show.setAccessible(true);show.invoke(activity,1);
        ((Runnable)field("poll")).run();await(()->{try{return ((Spinner)field("modemMode")).getCount()==2;}catch(Exception e){return false;}});
        assertEquals(1,((Spinner)field("modemMode")).getSelectedItemPosition());
        assertEquals(2,((Spinner)field("modemContent")).getSelectedItemPosition());
        assertFalse((boolean)field("modemEdited"));
        DeviceApi api=new DeviceApi(activity,base);assertTrue(api.post("/api/config",new JSONObject()).getBoolean("queued"));
    }
    @Test public void fixedNavigationAndIndependentPageScrolling() throws Exception {
        screenshot("android-navigation-top-host.png");
        View header=(View)field("fixedHeader"),notice=(View)field("notice");ScrollView scroll=(ScrollView)field("contentScroll");
        Button[] tabs=(Button[])field("pageTabs");assertEquals("RTL-SDR",tabs[0].getText());assertEquals("PSK Modem",tabs[1].getText());
        assertTrue(tabs[0].isSelected());assertFalse(tabs[1].isSelected());
        int[] headerBefore=new int[2],noticeBefore=new int[2],after=new int[2];
        header.getLocationOnScreen(headerBefore);notice.getLocationOnScreen(noticeBefore);
        scroll.scrollTo(0,350);int moved=scroll.getScrollY();assertTrue(moved>0);
        header.getLocationOnScreen(after);assertArrayEquals(headerBefore,after);
        notice.getLocationOnScreen(after);assertEquals(noticeBefore[1]-moved,after[1]);
        assertTrue(scroll.getTop()>=header.getBottom());assertFalse(scroll.isScrollbarFadingEnabled());
        screenshot("android-navigation-scrolled-host.png");
        tabs[1].performClick();Shadows.shadowOf(Looper.getMainLooper()).idle();
        assertTrue(tabs[1].isSelected());assertEquals(0,scroll.getScrollY());
        screenshot("android-navigation-modem-host.png");
        tabs[0].performClick();Shadows.shadowOf(Looper.getMainLooper()).idle();assertEquals(moved,scroll.getScrollY());
        header.getLocationOnScreen(after);assertArrayEquals(headerBefore,after);
        assertEquals(0,writes.get());
    }
    private void screenshot(String name) throws Exception {
        View view=activity.getWindow().getDecorView();view.measure(View.MeasureSpec.makeMeasureSpec(1233,View.MeasureSpec.EXACTLY),View.MeasureSpec.makeMeasureSpec(2673,View.MeasureSpec.EXACTLY));view.layout(0,0,1233,2673);
        Bitmap bitmap=Bitmap.createBitmap(1233,2673,Bitmap.Config.ARGB_8888);view.draw(new Canvas(bitmap));
        File out=new File("../../../build/android",name);out.getParentFile().mkdirs();try(FileOutputStream stream=new FileOutputStream(out)){bitmap.compress(Bitmap.CompressFormat.PNG,100,stream);}
    }
    private record Exchange(String method,String path,byte[] body,OutputStream out){}
    private interface RequestHandler {void handle(Exchange exchange) throws IOException;}
    private static final class FixtureServer {
        final ServerSocket socket;RequestHandler handler;
        FixtureServer() throws IOException {socket=new ServerSocket(0,8,InetAddress.getByName("127.0.0.1"));}
        private static String line(InputStream in) throws IOException {ByteArrayOutputStream out=new ByteArrayOutputStream();int c;while((c=in.read())!=-1&&c!='\n'){if(out.size()>8192)throw new IOException("Header too long");if(c!='\r')out.write(c);}return out.toString(StandardCharsets.US_ASCII);}
        void start(){Thread thread=new Thread(()->{while(!socket.isClosed())try(Socket client=socket.accept()){
            client.setSoTimeout(4000);InputStream in=client.getInputStream();String[] first=line(in).split(" ");int length=0;String h;while(!(h=line(in)).isEmpty()){if(h.toLowerCase(java.util.Locale.US).startsWith("content-length:"))length=Integer.parseInt(h.substring(15).trim());}
            if(length<0||length>1048576)throw new IOException("Body too long");byte[] body=in.readNBytes(length);handler.handle(new Exchange(first[0],first[1].split("\\?")[0],body,client.getOutputStream()));
        }catch(Exception ignored){}});thread.setDaemon(true);thread.start();}
        void stop(int ignored){try{socket.close();}catch(IOException e){throw new AssertionError(e);}}
    }
}
