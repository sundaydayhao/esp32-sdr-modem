package cn.sdrstudio.app;

import android.content.Context;
import java.io.*;
import java.nio.charset.StandardCharsets;
import org.json.JSONObject;

/** Device status translations share the LCD's source catalog. RF payload is untouched. */
final class UiText {
    private final JSONObject catalog;
    UiText(Context context) {
        JSONObject loaded=new JSONObject();
        try(InputStream in=context.getAssets().open("ui_zh.json")) {
            ByteArrayOutputStream bytes=new ByteArrayOutputStream();byte[] block=new byte[4096];int n;
            while((n=in.read(block))!=-1){if(bytes.size()+n>128*1024)throw new IOException("Catalog too large");bytes.write(block,0,n);}
            loaded=new JSONObject(new String(bytes.toByteArray(),StandardCharsets.UTF_8));
        }catch(Exception ignored){}
        catalog=loaded;
    }
    String get(String value){
        if(value.equals("Recording CU8 to SD; press Stop to save"))return "正在写入 SD；点击停止后完成保存";
        if(value.equals("Opening SD replay"))return "正在打开 SD 文件并校验";
        if(value.equals("SD replay / recorded IQ, not live RF"))return "正在重放 SD 历史 IQ";
        if(value.equals("Checking SD IQ CRC before replay"))return "正在校验 SD 文件 CRC";
        if(value.equals("SD replay complete"))return "SD 重放完成";
        return catalog.optString(value,value);
    }
}
