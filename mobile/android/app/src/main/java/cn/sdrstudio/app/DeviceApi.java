package cn.sdrstudio.app;

import android.content.Context;
import android.net.*;
import org.json.*;
import java.io.*;
import java.net.*;
import java.nio.charset.StandardCharsets;

/** Blocking I/O. Owned by MainActivity's single worker; never called on the UI thread. */
final class DeviceApi {
    private final ConnectivityManager connectivity;
    private volatile String base;
    DeviceApi(Context context, String address) {
        connectivity=(ConnectivityManager)context.getSystemService(Context.CONNECTIVITY_SERVICE);
        setAddress(address);
    }
    static String validateAddress(String input) {
        String value=input.trim(); if(!value.startsWith("http://"))value="http://"+value;
        try {
            URI u=new URI(value);
            if(!"http".equals(u.getScheme()) || u.getHost()==null || u.getUserInfo()!=null || u.getQuery()!=null || u.getFragment()!=null ||
                !(u.getPath()==null||u.getPath().isEmpty()||u.getPath().equals("/")))throw new IllegalArgumentException();
            String host=u.getHost();String[] parts=host.split("\\.");
            if(parts.length!=4)throw new IllegalArgumentException();
            for(String part:parts)if(!part.matches("[0-9]{1,3}")||Integer.parseInt(part)>255)throw new IllegalArgumentException();
            int a=Integer.parseInt(parts[0]),b=Integer.parseInt(parts[1]);
            if(!(a==10||a==192&&b==168||a==172&&b>=16&&b<=31||a==127))throw new IllegalArgumentException();
            if(u.getPort()==0||u.getPort()>65535)throw new IllegalArgumentException();
            return "http://"+host+(u.getPort()<0?"":":"+u.getPort());
        } catch(Exception e){throw new IllegalArgumentException("请输入设备的局域网 IPv4 地址，例如 192.168.4.1");}
    }
    void setAddress(String address){base=validateAddress(address);}
    String address(){return base;}
    DatagramSocket openAudioSocket() throws IOException {
        InetAddress target=InetAddress.getByName(URI.create(base).getHost());
        DatagramSocket socket=new DatagramSocket();
        try{
            for(Network n:connectivity.getAllNetworks()){
                NetworkCapabilities caps=connectivity.getNetworkCapabilities(n);LinkProperties props=connectivity.getLinkProperties(n);
                if(caps==null||props==null||!caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI))continue;
                boolean found=false;for(RouteInfo route:props.getRoutes())if(!route.isDefaultRoute()&&route.matches(target)){found=true;break;}
                if(found){n.bindSocket(socket);break;}
            }
            socket.setReceiveBufferSize(12*1024);socket.setSoTimeout(200);socket.connect(target,8081);return socket;
        }catch(Exception e){socket.close();throw new IOException("无法连接设备音频",e);}
    }
    private HttpURLConnection open(String path) throws IOException {
        URL url=new URL(base+path);URLConnection connection=null;
        // Select only the Wi-Fi network whose route covers this device. Do not bind
        // the process: cellular Internet can continue to load map tiles.
        InetAddress target=InetAddress.getByName(url.getHost());
        for(Network n:connectivity.getAllNetworks()) {
            NetworkCapabilities caps=connectivity.getNetworkCapabilities(n);
            LinkProperties props=connectivity.getLinkProperties(n);
            if(caps==null||props==null||!caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI))continue;
            for(RouteInfo route:props.getRoutes())if(!route.isDefaultRoute()&&route.matches(target)){
                connection=n.openConnection(url);break;
            }
            if(connection!=null)break;
        }
        if(connection==null)connection=url.openConnection();
        HttpURLConnection c=(HttpURLConnection)connection;c.setConnectTimeout(2500);c.setReadTimeout(3500);
        c.setInstanceFollowRedirects(false);c.setRequestProperty("User-Agent","SDRStudioAndroid/0.1");return c;
    }
    JSONObject get(String path) throws IOException,JSONException{return new JSONObject(new String(bytes(path,512*1024),StandardCharsets.UTF_8));}
    JSONArray array(String path) throws IOException,JSONException{return new JSONArray(new String(bytes(path,512*1024),StandardCharsets.UTF_8));}
    JSONObject post(String path,JSONObject body) throws IOException,JSONException {
        HttpURLConnection c=open(path);try {
            c.setRequestMethod("POST");c.setDoOutput(true);c.setRequestProperty("Content-Type","application/json");
            byte[] data=body.toString().getBytes(StandardCharsets.UTF_8);c.setFixedLengthStreamingMode(data.length);
            try(OutputStream out=c.getOutputStream()){out.write(data);}
            return new JSONObject(new String(response(c,512*1024),StandardCharsets.UTF_8));
        } finally{c.disconnect();}
    }
    byte[] bytes(String path,int limit) throws IOException {HttpURLConnection c=open(path);try{return response(c,limit);}finally{c.disconnect();}}
    record Received(byte[] bytes,String cursor,String epoch,String boot,String lost) {}
    Received receive(String cursor) throws IOException {
        if(!cursor.matches("[0-9]{1,20}"))throw new IOException("接收游标无效");
        HttpURLConnection c=open("/api/rx?cursor="+cursor);try {
            byte[] bytes=response(c,8192);String next=c.getHeaderField("X-Next-Cursor");
            if(next==null||!next.matches("[0-9]{1,20}"))throw new IOException("设备接收游标缺失");
            return new Received(bytes,next,c.getHeaderField("X-Content-Epoch"),c.getHeaderField("X-Boot-Id"),c.getHeaderField("X-Lost-Bytes"));
        }finally{c.disconnect();}
    }
    private static byte[] response(HttpURLConnection c,int limit) throws IOException {
        int code=c.getResponseCode();boolean success=code>=200&&code<300;InputStream stream=success?c.getInputStream():c.getErrorStream();
        if(stream==null)throw new IOException("设备返回 HTTP "+code);
        ByteArrayOutputStream out=new ByteArrayOutputStream();byte[] block=new byte[8192];
        try(InputStream in=stream){int n;while((n=in.read(block))!=-1){if(out.size()+n>limit)throw new IOException("设备响应超过上限");out.write(block,0,n);}}
        byte[] data=out.toByteArray();if(!success)throw new IOException("设备拒绝："+new String(data,StandardCharsets.UTF_8));return data;
    }
    long download(String file,OutputStream output,long expected) throws IOException {
        if(!file.matches("IQ[0-9]{6}\\.IQ"))throw new IOException("无效的 SD 文件名");
        HttpURLConnection c=open("/api/sdr/record?file="+file);c.setReadTimeout(12000);
        try {
            if(c.getResponseCode()!=200){response(c,4096);throw new IOException("下载失败");}
            long count=0;byte[] buf=new byte[8192];java.util.zip.CRC32 crc=new java.util.zip.CRC32();
            try(InputStream in=c.getInputStream()){int n;while((n=in.read(buf))!=-1){
                if(Thread.currentThread().isInterrupted())throw new InterruptedIOException("下载已取消");
                count+=n;if(count>expected)throw new IOException("文件长度超过设备目录记录");crc.update(buf,0,n);output.write(buf,0,n);
            }}
            if(count!=expected)throw new IOException("文件未完整接收："+count+" / "+expected);
            return crc.getValue();
        } finally{c.disconnect();}
    }
}
