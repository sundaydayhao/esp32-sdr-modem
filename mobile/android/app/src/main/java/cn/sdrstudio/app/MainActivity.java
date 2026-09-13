package cn.sdrstudio.app;

import android.app.*;
import android.os.*;
import android.content.*;
import android.graphics.*;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.provider.Settings;
import android.text.*;
import android.view.*;
import android.widget.*;
import org.json.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.concurrent.*;

public final class MainActivity extends Activity {
    static final int BG=0xff0b111b,PANEL=0xff131e2c,TEXT=0xffecf2fa,MUTED=0xff90a3bc,ACCENT=0xff5de0c0;
    private final Handler main=new Handler(Looper.getMainLooper());
    private final ExecutorService worker=Executors.newSingleThreadExecutor();
    private DeviceApi api;
    private WfmAudioPlayer phonePlayer;
    private CheckBox phoneListen;
    private TextView phoneAudioStatus;
    private LinearLayout phoneAudioBox;
    private UiText copy;
    private JSONObject state=new JSONObject(),config=ReceiverConfig.defaults();
    private JSONArray modemModes=new JSONArray(),files=new JSONArray();
    private boolean connected=false,polling=false,active=false,filling=false,edited=false,modemEdited=false,commandPending=false,simulation=false;
    private volatile int session=0;
    private int page=0,selection=3,bandIndex=0,imageVersion=-1;
    private int renderedModemContent=0,renderedModemMode=0;
    private String boot="",cursor="0",rxEpoch="",fileSignature="",downloadFile;
    private long downloadSize;
    private final ByteArrayOutputStream received=new ByteArrayOutputStream();
    private LinearLayout receiver,modem,frequencyBox,scanBox,bandBox,replayBox,aircraftList,contentBox;
    private TextView connection,notice,stats,details,aircraftDetail,rxText,modemStatus;
    private EditText frequency,start,stop,modemTone;
    private Spinner modes,bands,analysis,sdFile,replayMode,modemMode,modemContent;
    private Button apply,action,refresh,download,peak;
    private CheckBox monitor;
    private SeekBar volume;
    private SpectrumView spectrum;
    private AircraftMapView map;
    private ImageView image;
    private View mapPanel;
    private LinearLayout fixedHeader;
    private ScrollView contentScroll;
    private final Button[] pageTabs=new Button[2];
    private final int[] pageScrollY=new int[2];
    private final ArrayList<View> deviceControls=new ArrayList<>();
    private final Runnable poll=()->{if(!active)return;if(polling||commandPending){schedulePoll(300);return;}polling=true;int epoch=session;worker.execute(()->pollDevice(epoch));};
    private void schedulePoll(long delay){main.removeCallbacks(poll);if(active)main.postDelayed(poll,delay);}

    @Override public void onCreate(Bundle saved){
        super.onCreate(saved);copy=new UiText(this);api=new DeviceApi(this,getPreferences(0).getString("device","192.168.4.1"));
        if(BuildConfig.DEBUG&&getIntent().hasExtra("test_endpoint")){api.setAddress(getIntent().getStringExtra("test_endpoint"));simulation=true;}
        build();
        setVolumeControlStream(android.media.AudioManager.STREAM_MUSIC);
        phonePlayer=new WfmAudioPlayer(this,api,new WfmAudioPlayer.Listener(){
            public void status(String message){phoneAudioStatus.setText(message);}
            public void interrupted(){phoneListen.setChecked(false);}
        });
    }
    private int dp(float n){return Math.round(n*getResources().getDisplayMetrics().density);}
    private LinearLayout column(){LinearLayout v=new LinearLayout(this);v.setOrientation(LinearLayout.VERTICAL);return v;}
    private LinearLayout row(){LinearLayout v=new LinearLayout(this);v.setOrientation(LinearLayout.HORIZONTAL);v.setGravity(Gravity.CENTER_VERTICAL);return v;}
    private TextView text(String value,int size,int color){TextView t=new TextView(this);t.setText(value);t.setTextSize(size);t.setTextColor(color);t.setPadding(0,dp(3),0,dp(3));return t;}
    private void add(LinearLayout p,View v){p.addView(v,new LinearLayout.LayoutParams(-1,-2));}
    private GradientDrawable background(int color){GradientDrawable d=new GradientDrawable();d.setColor(color);d.setCornerRadius(dp(12));d.setStroke(dp(1),0xff26364a);return d;}
    private LinearLayout card(LinearLayout parent){LinearLayout box=column();box.setPadding(dp(14),dp(12),dp(14),dp(12));box.setBackground(background(PANEL));LinearLayout.LayoutParams lp=new LinearLayout.LayoutParams(-1,-2);lp.bottomMargin=dp(12);parent.addView(box,lp);return box;}
    private Button button(String label,Runnable click){Button b=new Button(this);b.setText(label);b.setAllCaps(false);b.setTextColor(TEXT);b.setTextSize(14);b.setMinHeight(dp(44));b.setBackgroundTintList(android.content.res.ColorStateList.valueOf(0xff23374a));b.setOnClickListener(v->click.run());return b;}
    private void equal(LinearLayout row,View view){LinearLayout.LayoutParams p=new LinearLayout.LayoutParams(0,dp(48),1);p.setMargins(dp(2),dp(2),dp(2),dp(2));row.addView(view,p);}
    private Spinner spinner(String[] entries){Spinner s=new Spinner(this,Spinner.MODE_DROPDOWN);s.setBackground(background(BG));s.setMinimumHeight(dp(48));setEntries(s,entries);return s;}
    private void setEntries(Spinner s,String[] entries){ArrayAdapter<String> a=new ArrayAdapter<>(this,android.R.layout.simple_spinner_dropdown_item,entries);s.setAdapter(a);}
    private EditText number(String value){EditText e=new EditText(this);e.setTextColor(TEXT);e.setTextSize(20);e.setSingleLine(true);e.setInputType(android.text.InputType.TYPE_CLASS_NUMBER|android.text.InputType.TYPE_NUMBER_FLAG_DECIMAL);e.setText(value);e.setSelectAllOnFocus(true);e.addTextChangedListener(new TextWatcher(){public void beforeTextChanged(CharSequence s,int st,int c,int a){}public void onTextChanged(CharSequence s,int st,int before,int count){if(!filling)edited=true;}public void afterTextChanged(Editable e){}});return e;}
    private void chosen(Spinner s,java.util.function.IntConsumer cb){s.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener(){public void onNothingSelected(AdapterView<?> p){}public void onItemSelected(AdapterView<?> p,View v,int position,long id){if(!filling)cb.accept(position);}});}
    private View control(View v){deviceControls.add(v);return v;}

    private void build(){
        filling=true;LinearLayout root=column();root.setBackgroundColor(BG);setContentView(root);
        fixedHeader=column();fixedHeader.setBackgroundColor(0xff101f2e);fixedHeader.setPadding(dp(14),dp(8),dp(14),dp(10));fixedHeader.setElevation(dp(6));add(root,fixedHeader);
        LinearLayout header=row();TextView title=text("SDR 工作台",24,TEXT);header.addView(title,new LinearLayout.LayoutParams(0,-2,1));header.addView(button("连接",this::connectionDialog));add(fixedHeader,header);
        connection=text("未连接设备",13,MUTED);add(fixedHeader,connection);
        LinearLayout tabs=row();pageTabs[0]=button("RTL-SDR",()->showPage(0));pageTabs[1]=button("PSK Modem",()->showPage(1));
        for(Button tab:pageTabs)equal(tabs,tab);add(fixedHeader,tabs);
        View divider=new View(this);divider.setBackgroundColor(0xff365164);root.addView(divider,new LinearLayout.LayoutParams(-1,dp(1)));
        contentScroll=new ScrollView(this);contentScroll.setFillViewport(true);contentScroll.setBackgroundColor(BG);
        contentScroll.setVerticalScrollBarEnabled(true);contentScroll.setScrollbarFadingEnabled(false);contentScroll.setScrollBarSize(dp(3));
        contentScroll.setScrollBarStyle(View.SCROLLBARS_INSIDE_OVERLAY);contentScroll.setVerticalFadingEdgeEnabled(true);contentScroll.setFadingEdgeLength(dp(14));
        if(Build.VERSION.SDK_INT>=29){GradientDrawable thumb=new GradientDrawable();thumb.setColor(MUTED);thumb.setCornerRadius(dp(2));contentScroll.setVerticalScrollbarThumbDrawable(thumb);}
        root.addView(contentScroll,new LinearLayout.LayoutParams(-1,0,1));LinearLayout body=column();body.setPadding(dp(14),dp(12),dp(14),dp(20));contentScroll.addView(body);
        notice=text("连接设备热点后，点击“连接”配置设备地址",13,0xffedb568);notice.setPadding(dp(10),dp(8),dp(10),dp(8));notice.setBackground(background(PANEL));
        LinearLayout.LayoutParams noticeLayout=new LinearLayout.LayoutParams(-1,-2);noticeLayout.bottomMargin=dp(12);body.addView(notice,noticeLayout);
        receiver=column();add(body,receiver);
        LinearLayout settings=card(receiver);add(settings,text("接收模式",13,MUTED));modes=spinner(ReceiverConfig.MODES);modes.setSelection(selection);add(settings,control(modes));
        chosen(modes,this::selectMode);
        bandBox=column();add(bandBox,text("中国大陆 / 接收预设",13,MUTED));bands=spinner(ReceiverConfig.BANDS);add(bandBox,control(bands));chosen(bands,n->{if(n!=bandIndex){bandIndex=n;if(selection==8)applyBand();}});
        analysis=spinner(new String[]{"OOK 脉冲","频谱分析","NFM 音频"});add(bandBox,control(analysis));chosen(analysis,n->{if(selection==8)applySubAnalysis(n);});add(settings,bandBox);
        frequencyBox=column();add(frequencyBox,text("接收频率 / MHz",13,MUTED));frequency=number("106.200000");add(frequencyBox,control(frequency));add(settings,frequencyBox);
        scanBox=column();add(scanBox,text("扫描范围 / MHz",13,MUTED));LinearLayout range=row();start=number("88");stop=number("108");equal(range,control(start));equal(range,control(stop));add(scanBox,range);add(settings,scanBox);
        replayBox=column();add(replayBox,text("SD IQ 文件 / 最新在前",13,MUTED));sdFile=spinner(new String[]{"暂无文件"});add(replayBox,control(sdFile));add(replayBox,text("重放解调方式",13,MUTED));replayMode=spinner(ReceiverConfig.REPLAY_LABELS);add(replayBox,control(replayMode));add(settings,replayBox);
        LinearLayout actions=row();apply=button("应用频率",this::applySettings);equal(actions,control(apply));equal(actions,button("接收设置",this::receiverDialog));add(settings,actions);
        monitor=new CheckBox(this);monitor.setText("声音监听（设备扬声器）");monitor.setTextColor(TEXT);monitor.setChecked(true);add(settings,control(monitor));monitor.setOnCheckedChangeListener((b,on)->{if(!filling&&connected)change(c->c.put("monitor",on));});
        volume=new SeekBar(this);volume.setMax(100);volume.setProgress(35);volume.setContentDescription("设备监听音量");add(settings,control(volume));volume.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener(){public void onStartTrackingTouch(SeekBar s){}public void onProgressChanged(SeekBar s,int p,boolean u){}public void onStopTrackingTouch(SeekBar s){if(connected)change(c->c.put("volume",s.getProgress()));}});
        phoneAudioBox=column();phoneListen=new CheckBox(this);phoneListen.setText("手机播放 WFM");phoneListen.setTextColor(ACCENT);add(phoneAudioBox,phoneListen);
        phoneAudioStatus=text("使用手机媒体音量调节",12,MUTED);add(phoneAudioBox,phoneAudioStatus);add(settings,phoneAudioBox);
        phoneListen.setOnCheckedChangeListener((b,on)->syncPhoneAudio());
        LinearLayout signal=card(receiver);add(signal,text("频谱 / 瀑布图     弱  ░▒▓  强",14,MUTED));spectrum=new SpectrumView(this);signal.addView(spectrum,new LinearLayout.LayoutParams(-1,dp(230)));stats=text("等待真实 USB IQ",12,MUTED);add(signal,stats);spectrum.setTag(signal);
        LinearLayout mapBox=card(receiver);mapPanel=mapBox;map=new AircraftMapView(this);map.selection(this::selectAircraft);mapBox.addView(map,new LinearLayout.LayoutParams(-1,dp(360)));
        LinearLayout mapButtons=row();equal(mapButtons,button("全部飞机",map::fit));equal(mapButtons,button("＋",()->map.changeZoom(1)));equal(mapButtons,button("－",()->map.changeZoom(-1)));add(mapBox,mapButtons);
        aircraftDetail=text("点选飞机可查看位置和飞行信息",13,TEXT);add(mapBox,aircraftDetail);aircraftList=column();add(mapBox,aircraftList);
        contentBox=card(receiver);details=text("等待接收数据",14,TEXT);add(contentBox,details);
        peak=button("查看最强信号",()->{long hz=state.optLong("scan_peak_hz");if(hz>0){selection=9;change(c->{c.put("mode",9);c.put("frequency_hz",hz);c.put("enabled",true);c.put("monitor",false);c.put("decode",false);});}});add(contentBox,control(peak));
        action=button("采集至 SD 卡",this::recordAction);add(contentBox,control(action));LinearLayout sdActions=row();refresh=button("刷新 SD",()->post("/api/sdr/sd",json("action","refresh")));download=button("导出 IQ",this::chooseDownload);equal(sdActions,control(refresh));equal(sdActions,control(download));add(contentBox,sdActions);
        modem=column();add(body,modem);LinearLayout mc=card(modem);add(mc,text("发送 / PTT 在设备端操作；RTL-SDR 仅接收",13,MUTED));
        modemContent=spinner(new String[]{"文本","图像","Codec2 语音"});add(mc,control(modemContent));modemMode=spinner(new String[]{"连接后加载调制模式"});add(mc,control(modemMode));
        modemTone=number("2000");add(mc,text("解码音调 / Hz",13,MUTED));add(mc,control(modemTone));
        modemTone.addTextChangedListener(new TextWatcher(){public void beforeTextChanged(CharSequence s,int st,int c,int a){}public void onTextChanged(CharSequence s,int st,int before,int count){if(!filling)modemEdited=true;}public void afterTextChanged(Editable e){}});
        chosen(modemContent,n->{if(n!=renderedModemContent)modemEdited=true;});chosen(modemMode,n->{if(n!=renderedModemMode)modemEdited=true;});
        add(mc,control(button("应用解码设置",this::configureModem)));
        modemStatus=text("等待设备状态",13,MUTED);add(mc,modemStatus);rxText=text("尚未收到解码文本",16,TEXT);rxText.setTextIsSelectable(true);add(mc,rxText);image=new ImageView(this);image.setAdjustViewBounds(true);add(mc,image);
        filling=false;showPage(0);layoutMode();enableControls();
    }
    private void showPage(int value){
        boolean changed=page!=value;if(changed)pageScrollY[page]=contentScroll.getScrollY();
        page=value;receiver.setVisibility(value==0?View.VISIBLE:View.GONE);modem.setVisibility(value==1?View.VISIBLE:View.GONE);
        for(int n=0;n<pageTabs.length;n++){
            boolean selected=n==value;pageTabs[n].setSelected(selected);pageTabs[n].setTextColor(selected?BG:TEXT);
            pageTabs[n].setBackgroundTintList(android.content.res.ColorStateList.valueOf(selected?ACCENT:0xff23374a));
        }
        if(changed)contentScroll.post(()->{if(page==value)contentScroll.scrollTo(0,pageScrollY[value]);});
    }
    private void layoutMode(){
        bandBox.setVisibility(selection==8?View.VISIBLE:View.GONE);frequencyBox.setVisibility(selection==7||selection==10?View.GONE:View.VISIBLE);scanBox.setVisibility(selection==7?View.VISIBLE:View.GONE);replayBox.setVisibility(selection==10?View.VISIBLE:View.GONE);
        mapPanel.setVisibility(selection==6?View.VISIBLE:View.GONE);((View)spectrum.getTag()).setVisibility(selection==6?View.GONE:View.VISIBLE);contentBox.setVisibility(selection==6?View.GONE:View.VISIBLE);
        action.setVisibility(selection>=7?View.VISIBLE:View.GONE);peak.setVisibility(selection==7?View.VISIBLE:View.GONE);refresh.setVisibility(selection==9||selection==10?View.VISIBLE:View.GONE);download.setVisibility(selection==9||selection==10?View.VISIBLE:View.GONE);
        monitor.setVisibility(selection<=4||selection==8?View.VISIBLE:View.GONE);volume.setVisibility(monitor.getVisibility());apply.setText(selection==10?"刷新 SD 卡":selection==7?"开始扫描":"应用频率");
        phoneAudioBox.setVisibility(selection==3?View.VISIBLE:View.GONE);
        filling=true;if(modes.getSelectedItemPosition()!=selection)modes.setSelection(selection);filling=false;
    }
    private interface EditConfig {void edit(JSONObject c) throws Exception;}
    private void change(EditConfig edit){
        if(!connected||commandPending)return;
        try{JSONObject c=new JSONObject(config.toString());edit.edit(c);post("/api/sdr",c);}catch(Exception e){error(e);}
    }
    private void selectMode(int value){
        if(value==selection)return;
        if(ReceiverConfig.busy(state)){notice.setText("请先停止录制或重放，再切换接收模式");layoutMode();return;}
        selection=value;layoutMode();
        if(value==10){if(connected)post("/api/sdr/sd",json("action","refresh"));return;}
        if(connected)try {JSONObject c=ReceiverConfig.select(config,value,bandIndex);post("/api/sdr",c);}catch(Exception e){error(e);}
    }
    private void applyBand(){try{JSONObject c=new JSONObject(config.toString());ReceiverConfig.applyBand(c,bandIndex);filling=true;analysis.setSelection(bandIndex==3?1:0);filling=false;post("/api/sdr",c);}catch(Exception e){error(e);}}
    private void applySubAnalysis(int n){if(!connected||commandPending)return;int mode=new int[]{8,5,4}[Math.max(0,Math.min(2,n))];if(config.optInt("mode")==mode)return;change(c->{c.put("mode",mode);c.put("decode",false);c.put("monitor",mode==4);});}
    private void applySettings(){if(selection==10){post("/api/sdr/sd",json("action","refresh"));return;}change(c->{c.put("frequency_hz",ReceiverConfig.mhz(frequency.getText().toString()));c.put("scan_start_hz",ReceiverConfig.mhz(start.getText().toString()));c.put("scan_stop_hz",ReceiverConfig.mhz(stop.getText().toString()));c.put("enabled",true);});}
    private static JSONObject json(String key,Object value){JSONObject j=new JSONObject();try{j.put(key,value);}catch(JSONException e){throw new AssertionError(e);}return j;}
    private void recordAction(){
        if(selection==10){if(state.optBoolean("replaying"))post("/api/sdr/sd",json("action","stop"));else try{JSONObject f=files.optJSONObject(sdFile.getSelectedItemPosition());if(f==null)throw new IllegalArgumentException("请选择 SD 文件");JSONObject j=json("action","play");j.put("file",f.getString("name"));j.put("mode",ReceiverConfig.REPLAY[replayMode.getSelectedItemPosition()]);post("/api/sdr/sd",j);}catch(Exception e){error(e);}return;}
        if(selection==7){change(c->c.put("enabled",!c.optBoolean("enabled")));return;}
        if(selection==8){selection=9;change(c->{c.put("mode",9);c.put("monitor",false);c.put("decode",false);});return;}
        post("/api/sdr/record",json("action",state.optBoolean("recording")?"stop":"start"));
    }
    private void post(String path,JSONObject body){
        if(!connected||commandPending)return;commandPending=true;notice.setText("正在应用到设备…");enableControls();final int epoch=session;
        worker.execute(()->{try{JSONObject result=api.post(path,body);main.post(()->{if(epoch!=session)return;commandPending=false;edited=false;update(result,!path.equals("/api/sdr/sd"));notice.setText(simulation?"HOST TEST / 模拟接口，非实物接收":"设置已在设备生效");enableControls();});}
        catch(Exception e){main.post(()->{if(epoch!=session)return;commandPending=false;restoreSelection();error(e);enableControls();});}});
    }
    private void restoreSelection(){selection=state.optBoolean("replay_source")?10:config.optInt("mode",3);fillFields();layoutMode();}
    private void pollDevice(int epoch){
        try{
            JSONObject info=null;if(!connected){info=api.get("/api/info");if(!"sdr-studio".equals(info.optString("product"))||info.optInt("api_version")!=1)throw new IOException("设备类型或 API 版本不匹配");}
            JSONObject s=api.get("/api/sdr");JSONArray planes=s.optJSONObject("config").optInt("mode")==6?api.get("/api/aircraft").optJSONArray("aircraft"):null;
            JSONArray available=null;JSONObject ms=null;DeviceApi.Received rx=null;byte[] bmp=null;
            if(page==1){if(modemModes.length()==0)available=api.array("/api/modes");ms=api.get("/api/status");
                if(ms.optInt("content_type")==0)rx=api.receive(ms.optString("boot_id").equals(boot)?cursor:"0");
                if(ms.optInt("content_type")==1&&ms.optInt("image_version")!=imageVersion)bmp=api.bytes("/api/image",64*1024);}
            JSONObject infoFinal=info,modemFinal=ms;JSONArray planesFinal=planes,modesFinal=available;DeviceApi.Received rxFinal=rx;byte[] bmpFinal=bmp;
            main.post(()->{if(epoch!=session)return;boolean initial=!connected;connected=true;
                if(infoFinal!=null)simulation=simulation||"simulation".equals(infoFinal.optString("source"));
                update(s,initial);if(planesFinal!=null)updateAircraft(planesFinal);if(modemFinal!=null)updateModem(modemFinal,modesFinal,rxFinal,bmpFinal);
                connection.setText((simulation?"HOST TEST · ":"已连接 · ")+api.address());connection.setTextColor(ACCENT);if(initial)notice.setText(simulation?"HOST TEST / 模拟接口，非实物接收":"设备已连接，显示真实接收状态");enableControls();});
        }catch(Exception e){main.post(()->{if(epoch!=session)return;connected=false;connection.setText("未连接 · "+api.address());connection.setTextColor(0xffedb568);notice.setText("连接失败："+friendly(e)+"。请确认已连接设备热点。");spectrum.stale();map.stale();enableControls();});}
        finally{main.post(()->{if(epoch!=session)return;polling=false;schedulePoll(connected?800:2500);});}
    }
    private void update(JSONObject value,boolean force){
        String next=value.optString("boot_id");if(!next.equals(boot)){boot=next;cursor="0";rxEpoch="";received.reset();imageVersion=-1;force=true;map.update(new JSONArray());}
        state=value;JSONObject c=value.optJSONObject("config");if(c==null)return;config=c;
        syncPhoneAudio();
        int deviceMode=c.optInt("mode");boolean keepSub=selection==8&&(deviceMode==8||deviceMode==5||deviceMode==4);
        if(state.optBoolean("replay_source"))selection=10;else if(!keepSub&&!(selection==10&&!force))selection=deviceMode;
        if(force||!edited)fillFields();layoutMode();spectrum.update(value);
        stats.setText(String.format(Locale.US,"%.3f MHz · %.0f kS/s · IQ 丢块 %d / PCM %d",value.optDouble("active_frequency_hz")/1e6,value.optDouble("sample_rate_hz")/1e3,value.optLong("iq_drops"),value.optLong("pcm_drops")));
        if(selection==10){details.setText("SD IQ 重放 / 历史采集数据\n"+value.optString("sd_file")+String.format(Locale.US,"\n%.2f / %.2f 秒",value.optDouble("replay_bytes")/576000,value.optDouble("replay_total")/576000)+"\n"+copy.get(value.optString("sd_message")));}
        else if(selection==9)details.setText(String.format(Locale.US,"SD IQ 采集 · 288 kS/s CU8\n%.2f 秒 / %.2f MiB\n丢样次数 %d · SD 剩余 %d MiB\n%s",value.optDouble("record_bytes")/576000,value.optDouble("record_bytes")/1048576,value.optLong("record_gaps"),value.optLong("sd_free_mb"),copy.get(value.optString("record_status"))));
        else if(selection==7)details.setText(String.format(Locale.US,"扫描 %d / %d · 第 %d 轮\n覆盖 %d%% · 最强 %.6f MHz\n峰值保持 / 实测 USB IQ",value.optLong("scan_step"),value.optLong("scan_steps"),value.optLong("scan_sweeps"),value.optLong("scan_coverage"),value.optDouble("scan_peak_hz")/1e6));
        else if(selection==8)details.setText(value.optBoolean("streaming")?("Sub-GHz 实时接收\n"+(deviceMode==8?(value.optLong("ook_frames")==0?"等待 OOK 脉冲":value.optLong("ook_frames")+" 组脉冲 · "+value.optLong("ook_pulses")+" 个边沿\n"+(value.optLong("ook_age_ms")<=5000?value.optString("pulses"):"等待新脉冲")):deviceMode==4?"NFM 音频监听":"频谱分析")+"\n尚未解码具体设备协议"):"等待实时 USB IQ");
        else details.setText((value.optBoolean("streaming")?"实时 USB 接收":"等待接收棒")+"\n"+(ReceiverConfig.audio(deviceMode)?"48 kHz 单声道 · 音频电平 "+value.optInt("audio_level")+" / 32768":"频谱分析")+"\n"+copy.get(value.optString("usb_status")));
        action.setText(selection==10?(value.optBoolean("replaying")?"停止 SD 重放":"播放 SD 文件"):selection==7?(c.optBoolean("enabled")?"暂停扫描":"继续扫描"):selection==8?"切换到 IQ 采集":value.optBoolean("record_saving")?"正在保存 SD…":value.optBoolean("recording")?"停止并保存到 SD":"采集至 SD 卡");
        JSONArray list=value.optJSONArray("sd_files");files=list==null?new JSONArray():list;String signature=files.toString();if(!signature.equals(fileSignature)){
            String selected=sdFile.getSelectedItem()==null?"":sdFile.getSelectedItem().toString();String[] names=new String[Math.max(1,files.length())];int index=0;names[0]="暂无 SD IQ 文件";for(int i=0;i<files.length();i++){names[i]=files.optJSONObject(i).optString("name");if(names[i].equals(selected))index=i;}
            filling=true;setEntries(sdFile,names);sdFile.setSelection(index);filling=false;fileSignature=signature;
        }
    }
    private void fillFields(){filling=true;frequency.setText(String.format(Locale.US,"%.6f",config.optDouble("frequency_hz")/1e6));start.setText(String.format(Locale.US,"%.6f",config.optDouble("scan_start_hz")/1e6));stop.setText(String.format(Locale.US,"%.6f",config.optDouble("scan_stop_hz")/1e6));monitor.setChecked(config.optBoolean("monitor"));if(!volume.isPressed())volume.setProgress(config.optInt("volume",35));
        if(selection==8){long hz=config.optLong("frequency_hz");for(int n=0;n<ReceiverConfig.PRESETS.length;n++)if(hz>=ReceiverConfig.PRESETS[n][1]&&hz<=ReceiverConfig.PRESETS[n][2]){bandIndex=n;bands.setSelection(n);break;}int m=config.optInt("mode");analysis.setSelection(m==4?2:m==5?1:0);}
        filling=false;}
    private void updateAircraft(JSONArray list){map.update(list);aircraftList.removeAllViews();if(list.length()==0){add(aircraftList,text("尚未收到飞机报文",14,MUTED));return;}
        for(int i=0;i<list.length();i++){JSONObject a=list.optJSONObject(i);if(a==null)continue;String callsign=a.optString("callsign").trim();String label=(callsign.isEmpty()?a.optString("icao"):callsign+" · "+a.optString("icao"))+"\n"+(a.has("altitude_feet")?a.optInt("altitude_feet")+" ft":"高度未知")+" · "+(AircraftMapView.valid(a)?"位置有效":"暂无有效位置");add(aircraftList,button(label,()->{selectAircraft(a);map.focus(a);}));}
    }
    private void selectAircraft(JSONObject a){aircraftDetail.setText(a.optString("callsign")+" · "+a.optString("icao")+"\n"+(AircraftMapView.valid(a)?String.format(Locale.US,"纬度 %.6f / 经度 %.6f (WGS84)",a.optDouble("latitude"),a.optDouble("longitude")):"尚无有效位置")+"\n高度 "+(a.has("altitude_feet")?a.optInt("altitude_feet")+" ft":"未知")+" · 速度 "+(a.has("speed_knots")?a.optInt("speed_knots")+" kt":"未知")+" · 航向 "+(a.has("heading_degrees")?a.optInt("heading_degrees")+"°":"未知"));}
    private void updateModem(JSONObject s,JSONArray available,DeviceApi.Received rx,byte[] bmp){
        if(available!=null){modemModes=available;String[] names=new String[available.length()];for(int i=0;i<names.length;i++)names[i]=available.optJSONObject(i).optString("name");filling=true;setEntries(modemMode,names);filling=false;}
        if(!modemEdited){filling=true;renderedModemContent=s.optInt("content_type");modemContent.setSelection(renderedModemContent);for(int i=0;i<modemModes.length();i++)if(modemModes.optJSONObject(i).optInt("id")==s.optInt("mode_id")){renderedModemMode=i;modemMode.setSelection(i);}modemTone.setText(""+s.optInt("tone_hz",2000));filling=false;}
        modemStatus.setText(s.optString("mode")+" · "+s.optString("role")+" · "+s.optInt("tone_hz")+" Hz\nCodec2 解码 "+s.optInt("decoded_frames")+" 帧 / CRC 错误 "+s.optInt("voice_crc_errors")+"\nCPU "+s.optDouble("dsp_load_percent")+"%");
        if(rx!=null){if(!Objects.equals(rx.epoch(),rxEpoch)||!Objects.equals(rx.boot(),boot)){received.reset();rxEpoch=rx.epoch();}cursor=rx.cursor();try{received.write(rx.bytes());}catch(IOException ignored){}
            byte[] all=received.toByteArray();if(all.length>16384){received.reset();received.write(all,all.length-16384,16384);}rxText.setText(received.size()==0?"尚未收到解码文本":new String(received.toByteArray(),StandardCharsets.UTF_8));if(!"0".equals(rx.lost()))notice.setText("接收文本有溢出："+rx.lost()+" 字节");}
        image.setVisibility(s.optInt("content_type")==1?View.VISIBLE:View.GONE);rxText.setVisibility(s.optInt("content_type")==0?View.VISIBLE:View.GONE);if(bmp!=null){Bitmap picture=BitmapFactory.decodeByteArray(bmp,0,bmp.length);if(picture!=null){image.setImageBitmap(picture);imageVersion=s.optInt("image_version");}}
    }
    private void configureModem(){if(!connected||commandPending)return;JSONObject m=modemModes.optJSONObject(modemMode.getSelectedItemPosition());if(m==null)return;try{JSONObject c=json("mode",m.getInt("id"));c.put("content_type",modemContent.getSelectedItemPosition());int tone=Integer.parseInt(modemTone.getText().toString());if(tone<100||tone>3500)throw new IllegalArgumentException("音调须在 100–3500 Hz 内");c.put("tone_hz",tone);commandPending=true;enableControls();int epoch=session;worker.execute(()->{try{api.post("/api/config",c);main.post(()->{if(epoch==session){commandPending=false;modemEdited=false;notice.setText("解码设置已提交");enableControls();}});}catch(Exception e){main.post(()->{if(epoch==session){commandPending=false;error(e);enableControls();}});}});}catch(Exception e){error(e);}}
    private void syncPhoneAudio(){
        if(phonePlayer==null)return;
        boolean wfm=config.optBoolean("enabled")&&config.optInt("mode")==3&&!state.optBoolean("replay_source");
        if(active&&connected&&wfm&&phoneListen.isChecked()){if(!phonePlayer.running())phonePlayer.start();}
        else{phonePlayer.stop();phoneAudioStatus.setText(!phoneListen.isChecked()?"使用手机媒体音量调节":!wfm?"切回 WFM 后恢复手机播放":"等待设备连接后恢复播放");}
    }
    private void enableControls(){boolean enabled=connected&&!commandPending;for(View v:deviceControls)v.setEnabled(enabled);
        syncPhoneAudio();phoneListen.setEnabled(connected||phoneListen.isChecked());
        if(!enabled)return;boolean busy=ReceiverConfig.busy(state);modes.setEnabled(!busy);bands.setEnabled(!busy);analysis.setEnabled(!busy);apply.setEnabled(!busy);frequency.setEnabled(!busy);start.setEnabled(!busy);stop.setEnabled(!busy);
        peak.setEnabled(state.optLong("scan_peak_hz")>0&&!busy);refresh.setEnabled(state.optInt("sd_state")==0);download.setEnabled(files.length()>0&&state.optInt("sd_state")==0);
        sdFile.setEnabled(state.optInt("sd_state")==0);replayMode.setEnabled(state.optInt("sd_state")==0);
        if(selection==9)action.setEnabled(state.optBoolean("recording")||state.optBoolean("sd_mounted")&&state.optBoolean("streaming")&&!state.optBoolean("record_saving")&&!state.optBoolean("replay_source"));
        if(selection==10)action.setEnabled(state.optBoolean("replaying")||state.optBoolean("sd_mounted")&&files.length()>0&&state.optInt("sd_state")==0);
    }
    private void receiverDialog(){LinearLayout box=column();box.setPadding(dp(20),dp(10),dp(20),0);Spinner front=spinner(ReceiverConfig.FRONTENDS);front.setSelection(config.optInt("frontend"));add(box,text("接收前端",14,MUTED));add(box,front);EditText lo=number(String.format(Locale.US,"%.6f",config.optDouble("converter_hz")/1e6));add(box,text("上变频器本振 / MHz",14,MUTED));add(box,lo);CheckBox decode=new CheckBox(this);decode.setText("USB / LSB 数字通信解码");decode.setChecked(config.optBoolean("decode"));add(box,decode);
        new AlertDialog.Builder(this).setTitle("接收设置").setView(box).setNegativeButton("取消",null).setPositiveButton("应用",(d,w)->change(c->{c.put("frontend",front.getSelectedItemPosition());c.put("converter_hz",ReceiverConfig.mhz(lo.getText().toString()));c.put("decode",decode.isChecked());})).show();}
    private void connectionDialog(){LinearLayout box=column();box.setPadding(dp(20),dp(10),dp(20),0);add(box,text("连接设备热点 ESP32-SDR\n密码：pskmodem32\nApp 通过 Wi-Fi 直接连接设备，无需浏览器。",14,TEXT));EditText address=new EditText(this);address.setSingleLine(true);address.setText(api.address().substring(7));add(box,address);add(box,button("打开 Wi-Fi 设置",()->startActivity(new Intent(Settings.ACTION_WIFI_SETTINGS))));add(box,button("地图来源与许可",()->new AlertDialog.Builder(this).setTitle("地图").setMessage("街道底图：OpenStreetMap\n离线道路和地名：深圳及周边区域\n© OpenStreetMap contributors · ODbL\n飞机坐标：设备实际 ADS-B 接收，WGS84\n联网地图可能使用移动数据；无网时显示已缓存瓦片和离线区域底图。").setPositiveButton("查看许可",(d,w)->startActivity(new Intent(Intent.ACTION_VIEW,Uri.parse("https://www.openstreetmap.org/copyright")))).setNegativeButton("关闭",null).show()));
        new AlertDialog.Builder(this).setTitle("连接设备").setView(box).setNegativeButton("取消",null).setPositiveButton("连接",(d,w)->{try{String value=DeviceApi.validateAddress(address.getText().toString());session++;connected=false;polling=false;commandPending=false;modemModes=new JSONArray();boot="";cursor="0";received.reset();spectrum.stale();map.stale();getPreferences(0).edit().putString("device",value).apply();worker.execute(()->api.setAddress(value));main.removeCallbacks(poll);main.post(poll);enableControls();}catch(Exception e){error(e);}}).show();}
    private void chooseDownload(){if(files.length()==0)return;String[] names=new String[files.length()];for(int i=0;i<names.length;i++)names[i]=files.optJSONObject(i).optString("name");new AlertDialog.Builder(this).setTitle("导出 IQ 与元数据（SigMF ZIP）").setItems(names,(d,n)->{downloadFile=names[n];downloadSize=files.optJSONObject(n).optLong("bytes");Intent intent=new Intent(Intent.ACTION_CREATE_DOCUMENT);intent.addCategory(Intent.CATEGORY_OPENABLE);intent.setType("application/zip");intent.putExtra(Intent.EXTRA_TITLE,downloadFile.replace(".IQ",".zip"));startActivityForResult(intent,10);}).setNegativeButton("取消",null).show();}
    @Override protected void onActivityResult(int request,int result,Intent data){super.onActivityResult(request,result,data);if(request!=10||result!=RESULT_OK||data==null||data.getData()==null)return;Uri uri=data.getData();String file=downloadFile;long bytes=downloadSize;if(!connected||commandPending){notice.setText("下载未开始：设备未连接或正在操作");return;}
        commandPending=true;notice.setText("正在导出 IQ，请保持 Wi-Fi 连接…");enableControls();int epoch=session;
        worker.execute(()->{try(OutputStream out=getContentResolver().openOutputStream(uri,"wt")){if(out==null)throw new IOException("无法打开目标文件");
            JSONObject metadata=api.get("/api/sdr/record-meta?file="+file);String expected=metadata.getJSONObject("global").optString("sdr:crc32");
            if(!expected.matches("[0-9a-fA-F]{8}"))throw new IOException("设备缺少 IQ 校验信息，请更新固件");
            try(java.util.zip.ZipOutputStream zip=new java.util.zip.ZipOutputStream(out)){
                zip.setLevel(0);zip.putNextEntry(new java.util.zip.ZipEntry(file.replace(".IQ",".sigmf-data")));
                long crc=api.download(file,zip,bytes);if(crc!=Long.parseLong(expected,16))throw new IOException("IQ CRC 校验失败，导出已取消");zip.closeEntry();
                zip.putNextEntry(new java.util.zip.ZipEntry(file.replace(".IQ",".sigmf-meta")));zip.write(metadata.toString(2).getBytes(StandardCharsets.UTF_8));zip.closeEntry();
            }main.post(()->{if(epoch==session){commandPending=false;notice.setText("已导出 "+file+" 与 SigMF 元数据，CRC 校验通过");enableControls();}});}catch(Exception e){try{android.provider.DocumentsContract.deleteDocument(getContentResolver(),uri);}catch(Exception ignored){}main.post(()->{if(epoch==session){commandPending=false;error(e);enableControls();}});}});
    }
    private static String friendly(Exception e){if(e instanceof java.net.SocketTimeoutException)return "连接超时";return e.getMessage()==null?e.getClass().getSimpleName():e.getMessage();}
    private void error(Exception e){notice.setText(friendly(e));}
    @Override protected void onResume(){super.onResume();active=true;syncPhoneAudio();main.removeCallbacks(poll);main.post(poll);}
    @Override protected void onPause(){active=false;if(phonePlayer!=null)phonePlayer.stop();main.removeCallbacks(poll);super.onPause();}
    @Override protected void onDestroy(){active=false;if(phonePlayer!=null)phonePlayer.stop();session++;main.removeCallbacksAndMessages(null);worker.shutdownNow();map.close();super.onDestroy();}
}
