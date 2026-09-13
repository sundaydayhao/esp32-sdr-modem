/* Shared receiver workspace controls; aircraft originate only from /api/aircraft. */
const dashboard = (() => {
  const $ = id => document.getElementById(id);
  const tabs = ['radioTab', 'adsbTab', 'subTab', 'captureTab'];
  let workspace = 0, presets = [], coast = [], planes = [], selected = '', auto = true;
  let view = {lat: 35, lon: 105, span: 110}, drag = null;
  const map = $('aircraftMap'), ctx = map.getContext('2d');
  const wrap = n => ((n + 180) % 360 + 360) % 360 - 180;
  const valid = a => a.has_position && Number.isFinite(a.latitude) && Number.isFinite(a.longitude) && Math.abs(a.latitude) <= 90 && Math.abs(a.longitude) <= 180;
  const project = (lat, lon) => [map.width / 2 + wrap(lon - view.lon) * map.width / view.span, map.height / 2 - (lat - view.lat) * map.width / view.span];
  function layout() {
    tabs.forEach((id, n) => $(id).setAttribute('aria-pressed', String(n === workspace)));
    $('aircraftPanel').hidden = workspace !== 1;
    $('radioData').hidden = workspace === 1;
    $('presetPanel').hidden = workspace !== 2;
    $('capturePanel').hidden = workspace !== 3;
  }
  function modeChanged(m) {
    if (m === 6) workspace = 1;
    else if (m === 7 || m === 9) workspace = 3;
    else if (m === 8) workspace = 2;
    else if (workspace !== 2 || ![4,5,7,9].includes(m)) workspace = m === 7 || m === 9 ? 3 : 0;
    layout();
  }
  function markDraft() { $('form').dispatchEvent(new Event('input', {bubbles: true})); }
  function selectPreset() {
    const p = presets[Number($('bandPreset').value)];
    if (!p) return;
    $('mode').selectedIndex = p.mode;
    $('frontend').selectedIndex = 0;
    $('frequency').value = (p.center_hz / 1e6).toFixed(6);
    $('scanStart').value = (p.start_hz / 1e6).toFixed(6);
    $('scanStop').value = (p.stop_hz / 1e6).toFixed(6);
    $('decode').checked = $('monitor').checked = false;
    $('enabled').checked = true;
    $('subAnalysis').value = String(p.mode);
    $('bandInfo').textContent = p.usage + '\n' + p.analysis;
    workspace = 2; layout(); markDraft();
    $('message').textContent = '已选择接收预设，点击“应用设置”调谐。';
  }
  tabs.forEach((id, n) => $(id).addEventListener('click', () => {
    workspace = n;
    const m = [3, 6, 8, 9][n];
    $('mode').selectedIndex = m;
    $('mode').dispatchEvent(new Event('change', {bubbles: true}));
    if (n === 2) selectPreset();
    markDraft(); layout();
  }));
  $('bandPreset').addEventListener('change', selectPreset);
  function selectAnalysis(mode) {
    $('subAnalysis').value=String(mode);$('mode').selectedIndex=mode;
    $('enabled').checked=true;$('decode').checked=false;$('monitor').checked=mode===4;
    if(mode===7||mode===9)$('captureKind').value=String(mode);
    $('captureStart').value=$('scanStart').value;$('captureStop').value=$('scanStop').value;
    modeChanged(mode);markDraft();
    $('message').textContent='已选择分析方式，点击“应用设置”连接 RTL-SDR。';
  }
  $('subAnalysis').addEventListener('change',()=>selectAnalysis(Number($('subAnalysis').value)));
  $('subCapture').onclick=()=>selectAnalysis(9);$('subScan').onclick=()=>selectAnalysis(7);
  function fit() {
    const a = planes.filter(valid); if (!a.length) return;
    const ref = a[0].longitude, lons = a.map(p => wrap(p.longitude - ref)), lats = a.map(p => p.latitude);
    const lo = Math.min(...lons), hi = Math.max(...lons), bottom = Math.min(...lats), top = Math.max(...lats);
    view = {lon: wrap(ref + (lo + hi) / 2), lat: (bottom + top) / 2, span: Math.min(360, Math.max(2, (hi-lo)*1.5, (top-bottom)*2.8))};
  }
  function choose(a) {
    selected = a.icao;
    if (valid(a)) { view = {lat: a.latitude, lon: a.longitude, span: 4}; auto = false; }
    rows(); paint();
  }
  function rows() {
    const list = $('aircraftRows'); list.replaceChildren();
    if (!planes.length) { const p=document.createElement('p');p.textContent='暂无飞机。应用 ADS-B 设置并连接接收棒与 1090 MHz 天线。';list.append(p); }
    for (const a of planes) {
      const b = document.createElement('button'); b.type = 'button';
      b.setAttribute('aria-pressed', String(a.icao === selected));
      b.textContent = (a.callsign || '无呼号') + '  ' + a.icao + '\n' + (a.altitude_feet ?? '—') + ' ft  /  ' + (a.speed_knots === undefined ? '—' : Math.round(a.speed_knots)) + ' kt\n' + (valid(a) ? '有效坐标' : '等待新坐标') + '  /  ' + Math.round(a.age_ms / 1000) + ' 秒前';
      b.addEventListener('click', () => choose(a)); list.append(b);
    }
    const a = planes.find(p => p.icao === selected);
    $('mapDetail').textContent = a ? (a.callsign || a.icao) + '\n' + (valid(a) ? a.latitude.toFixed(5) + ', ' + a.longitude.toFixed(5) : '暂无新鲜位置，不在地图上绘制') + '\n高度 ' + (a.altitude_feet ?? '—') + ' ft / 航向 ' + (a.heading_degrees === undefined ? '—' : Math.round(a.heading_degrees)) + '°' : '点选飞机查看位置；无有效坐标的飞机仅显示在列表中。';
  }
  function paint() {
    const w=map.width,h=map.height;
    ctx.fillStyle='#0b1723';ctx.fillRect(0,0,w,h);
    const step=view.span>80?30:view.span>20?10:view.span>5?2:view.span>1?.5:.1;
    ctx.strokeStyle='#203247';ctx.lineWidth=1;ctx.beginPath();
    for(let lon=-180;lon<180;lon+=step){const [x]=project(view.lat,lon);ctx.moveTo(x,0);ctx.lineTo(x,h);}
    for(let lat=-90;lat<=90;lat+=step){const [,y]=project(lat,view.lon);ctx.moveTo(0,y);ctx.lineTo(w,y);}ctx.stroke();
    ctx.strokeStyle='#526f8e';ctx.beginPath();
    for(const path of coast){let before=null;for(const [lon,lat] of path){const p=project(lat/100,lon/100);if(before&&Math.abs(p[0]-before[0])<w*180/view.span)ctx.lineTo(...p);else ctx.moveTo(...p);before=p;}}ctx.stroke();
    let count=0;ctx.font='16px system-ui';
    for(const a of planes.filter(valid)){
      count++;const [x,y]=project(a.latitude,a.longitude);if(x<0||x>w||y<0||y>h)continue;
      const angle=(a.heading_degrees ?? 0)*Math.PI/180;
      ctx.save();ctx.translate(x,y);ctx.rotate(angle);ctx.fillStyle=a.icao===selected?'#f6cf80':'#5de0c0';ctx.beginPath();ctx.moveTo(0,-9);ctx.lineTo(6,7);ctx.lineTo(0,3);ctx.lineTo(-6,7);ctx.closePath();ctx.fill();ctx.restore();
      ctx.fillStyle='#ecf2fa';ctx.fillText(a.callsign||a.icao,Math.min(w-100,x+12),Math.max(18,y-8));
    }
    if(!count){ctx.fillStyle='#b6c6da';ctx.textAlign='center';ctx.fillText('等待收到有效的飞机坐标',w/2,h/2);ctx.textAlign='left';}
    $('mapCaption').textContent='离线海岸线 · 中心 '+view.lat.toFixed(2)+', '+view.lon.toFixed(2)+' · '+count+' 架有坐标 · Natural Earth';
  }
  $('mapFit').addEventListener('click',()=>{auto=true;selected='';fit();rows();paint();});
  for(const [id,factor] of [['mapIn',.5],['mapOut',2]])$(id).addEventListener('click',()=>{auto=false;view.span=Math.max(.25,Math.min(360,view.span*factor));paint();});
  map.addEventListener('pointerdown',e=>{drag={x:e.clientX,y:e.clientY,startX:e.clientX,startY:e.clientY};map.setPointerCapture(e.pointerId);});
  map.addEventListener('pointermove',e=>{if(!drag)return;const rect=map.getBoundingClientRect();view.lon=wrap(view.lon-(e.clientX-drag.x)*view.span/rect.width);view.lat=Math.max(-85,Math.min(85,view.lat+(e.clientY-drag.y)*view.span/rect.width));drag.x=e.clientX;drag.y=e.clientY;auto=false;paint();});
  map.addEventListener('pointerup',e=>{if(drag&&Math.hypot(e.clientX-drag.startX,e.clientY-drag.startY)<6){const r=map.getBoundingClientRect(),px=(e.clientX-r.left)*map.width/r.width,py=(e.clientY-r.top)*map.height/r.height;const a=planes.filter(valid).find(a=>{const [x,y]=project(a.latitude,a.longitude);return Math.hypot(x-px,y-py)<24;});if(a)choose(a);}drag=null;});
  map.addEventListener('pointercancel',()=>drag=null);
  async function init() {
    try {
      const [mr,pr]=await Promise.all([fetch('/map-data.json'),fetch('/api/presets')]);
      if(!mr.ok||!pr.ok)throw Error('底图或频段目录读取失败');
      coast=(await mr.json()).paths;presets=(await pr.json()).presets;
      const select=$('bandPreset');select.replaceChildren();presets.forEach((p,n)=>{const o=document.createElement('option');o.value=n;o.textContent=p.name;select.append(o);});select.value='0';select.disabled=false;
      $('bandInfo').textContent=presets[0].usage+'\n'+presets[0].analysis;paint();
    } catch(e) { $('mapCaption').textContent=e.message;$('bandInfo').textContent=e.message; }
  }
  async function update(s,draft) {
    if(!draft){
      if(s.replay_source){workspace=3;layout();}else modeChanged(s.config.mode);
      const n=presets.findIndex(p=>s.config.frequency_hz>=p.start_hz&&s.config.frequency_hz<=p.stop_hz);
      if(n>=0){$('bandPreset').value=String(n);$('bandInfo').textContent=presets[n].usage+'\n'+presets[n].analysis;}
      if(!s.replay_source&&n>=0&&[4,5,8].includes(s.config.mode)){workspace=2;layout();}
      $('subAnalysis').value=String(s.config.mode);
    }
    const live=s.config.enabled&&s.streaming;
    $('subStatus').textContent=(live?'RTL-SDR 正在接收':'等待 RTL-SDR 实时 IQ')+' · '+(s.active_frequency_hz/1e6).toFixed(6)+' MHz · '+(s.sample_rate_hz/1000).toFixed(0)+' kS/s · IQ 块 '+s.iq_blocks+' · 丢块 '+s.iq_drops;
    if(workspace!==1)return;
    if(s.config.mode!==6){planes=[];rows();paint();$('aircraftSummary').textContent='点击应用设置，切换到 1090 MHz ADS-B 接收。';return;}
    const r=await fetch('/api/aircraft',{cache:'no-store'});if(!r.ok)throw Error('飞机数据读取失败');
    const data=await r.json();planes=data.boot_id===s.boot_id?data.aircraft:[];
    if(auto)fit();rows();paint();$('aircraftSummary').textContent=planes.length+' 架飞机 / '+s.adsb_messages+' 条有效报文'+(s.streaming?'':' / 暂无射频数据');
  }
  function disconnected(){planes=[];rows();paint();$('subStatus').textContent='设备连接中断，等待 RTL-SDR 状态';}
  layout();init();
  return {update,modeChanged,disconnected,isSubGHz:()=>workspace===2,showCapture:()=>{workspace=3;layout();}};
})();
