// Run the production browser script against the documented HTTP response
// shapes. No radio hardware or network service is simulated as a live test.
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const path = require('node:path');
const html = fs.readFileSync(path.join(__dirname, '../main/sdr.html'), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const elements = new Map(), calls = [], timers = [], canvasClears = {};
function element(id) {
  if (!elements.has(id)) elements.set(id, {
    value: '', checked: false, selectedIndex: 0, textContent: '', hidden: false,
    children: [{}, {}, {}], handlers: {}, attributes: {}, style:{}, width: 768, height: 290,
    addEventListener(type, fn) { this.handlers[type] = fn; },
    dispatchEvent(e){if(this.handlers[e.type])this.handlers[e.type](e);},
    setAttribute(k,v){this.attributes[k]=v;},
    replaceChildren(){this.children=[];},append(o){this.children.push(o);},
    getContext() { return {
      clearRect(){canvasClears[id]=(canvasClears[id]||0)+1;}, fillRect(){}, beginPath(){}, moveTo(){}, lineTo(){}, stroke(){}, drawImage(){},
      save(){},restore(){},translate(){},rotate(){},closePath(){},fill(){},fillText(){},
      createImageData(w, h) { return { data: new Uint8ClampedArray(w * h * 4) }; }, putImageData(){}
    }; }
  });
  return elements.get(id);
}
let response = {
  boot_id: 'boot-one', config: {enabled:true, decode:true, monitor:true, mode:0, frontend:1,
    frequency_hz:7074000, converter_hz:125000000, volume:35, scan_start_hz:88000000, scan_stop_hz:108000000},
  generation:1, sequence:1, streaming:true, usb_status:'HOST TEST',active_frequency_hz:7074000,sample_rate_hz:288000,
  fft:Array(512).fill(10),scan:Array(512).fill(5),waveform:Array(64).fill(0),modem_content:0,
  modem_mode:'PSK125R',iq_blocks:10,iq_drops:0,pcm_drops:0,audio_drops:0,dsp_max_us:100,
  adsb_messages:0,aircraft:'',pulses:'',ook_frames:0,record_status:'',record_bytes:0,record_gaps:0,
  voice_decoded:90,voice_crc_errors:0,image_rows:32,
  sd_state:0,sd_mounted:true,sd_catalog_generation:1,sd_files:[],sd_free_mb:196,
  sd_message:'SD ready',sd_file:'',replay_source:false,replaying:false
};
let rejectPost = false, rxCounter = 0;
let nextElement=0;
let aircraftData=[{icao:'40621D',callsign:'HOST123',has_position:true,latitude:52.26,longitude:3.94,altitude_feet:38000,age_ms:500},
  {icao:'ABCDEF',callsign:'NOCPR',has_position:false,age_ms:900}];
const context = vm.createContext({
  document:{getElementById:element,createElement:()=>element('generated-'+nextElement++)}, TextDecoder, Math, Number, Uint8ClampedArray, Event,
  setTimeout(fn){timers.push(fn);},
  async fetch(url, options={}) {
    calls.push({url,options});
    if(url==='/map-data.json')return {ok:true,json:async()=>JSON.parse(fs.readFileSync(path.join(__dirname,'../main/map_data.json'),'utf8'))};
    if(url==='/api/presets')return {ok:true,json:async()=>({presets:[{name:'433.92 MHz',center_hz:433920000,start_hz:433050000,stop_hz:434790000,mode:8,usage:'Remote controls',analysis:'OOK pulses'},{name:'315.00 MHz',center_hz:315000000,start_hz:314000000,stop_hz:316000000,mode:8,usage:'Remote controls',analysis:'OOK pulses'}]})};
    if(url==='/api/aircraft')return {ok:true,json:async()=>({boot_id:response.boot_id,aircraft:structuredClone(aircraftData)})};
    if (options.method==='POST') {
      if (rejectPost) return {ok:false,text:async()=> 'HF needs Direct Q'};
      if (url==='/api/sdr') { response.config=JSON.parse(options.body);response.replay_source=false; ++response.generation; }
      if (url==='/api/sdr/sd') {
        const request=JSON.parse(options.body);
        if(request.action==='play') {response.replaying=response.replay_source=true;response.sd_file=request.file;
          response.config.mode=request.mode;response.sd_state=6;response.record_ready=false;}
        if(request.action==='stop') {response.replaying=false;response.sd_state=0;response.record_ready=true;response.streaming=false;}
      }
    }
    if(url.startsWith('/api/rx?cursor=')) {
      const text=rxCounter++?' next':'SDR test';
      return {ok:true,headers:{get:key=>key==='X-Content-Epoch'?'1':String(rxCounter*8)},
        arrayBuffer:async()=>new TextEncoder().encode(text).buffer};
    }
    return {ok:true,json:async()=>structuredClone(response)};
  }
});
vm.runInContext(fs.readFileSync(path.join(__dirname,'../main/dashboard.js'),'utf8'), context, {filename:'dashboard.js'});
vm.runInContext(script, context, {filename:'sdr.html'});
async function settle() { for(let n=0;n<6;n++)await new Promise(resolve=>setImmediate(resolve)); }
async function tick() { const fn=timers.shift();assert(fn);await fn();await settle(); }
(async()=>{
  await settle();assert.match(element('details').textContent,/SDR test/);
  assert(calls.some(c=>c.url==='/api/rx?cursor=0'));
  await tick();assert(calls.some(c=>c.url==='/api/rx?cursor=8'));
  element('mode').selectedIndex=6;element('mode').handlers.change();
  assert.equal(element('frequency').value,'1090');assert.equal(element('frontend').selectedIndex,0);
  assert.equal(element('decode').checked,false);
  await element('form').handlers.submit({preventDefault(){}});await settle();
  const posted=JSON.parse(calls.findLast(c=>c.options.method==='POST').options.body);
  assert.equal(posted.frequency_hz,1090000000);assert.equal(posted.mode,6);
  await tick();assert.equal(element('aircraftPanel').hidden,false);
  assert.equal(element('aircraftRows').children.length,2);
  assert.match(element('mapCaption').textContent,/1 架有坐标/);
  element('aircraftRows').children[0].handlers.click();assert.match(element('mapDetail').textContent,/52.26000, 3.94000/);
  aircraftData=[];await tick();assert.match(element('aircraftRows').children[0].textContent,/暂无飞机/);
  rejectPost=true;await element('form').handlers.submit({preventDefault(){}});
  assert.match(element('message').textContent,/HF needs Direct Q/);rejectPost=false;
  response.config.mode=0;response.config.decode=true;response.modem_content=2;
  await tick();assert.match(element('details').textContent,/Codec2 700C/);
  assert.match(element('details').textContent,/90/);
  response.boot_id='boot-two';response.modem_content=0;rxCounter=0;
  await tick();assert.equal(calls.findLast(c=>c.url.startsWith('/api/rx')).url,'/api/rx?cursor=0');
  response.config.enabled=false;await tick();
  element('subTab').handlers.click();element('bandPreset').value='1';element('bandPreset').handlers.change();
  assert.equal(element('frequency').value,'315.000000');assert.equal(element('presetPanel').hidden,false);
  await element('form').handlers.submit({preventDefault(){}});
  assert.equal(response.config.frequency_hz,315000000);assert.equal(response.config.decode,false);
  assert.equal(response.config.enabled,true);assert.equal(response.config.monitor,false);
  response.active_frequency_hz=315000000;response.ook_frames=0;response.streaming=true;await tick();
  assert.match(element('subStatus').textContent,/315.000000 MHz/);assert.match(element('details').textContent,/等待 OOK/);
  response.ook_frames=3;response.ook_pulses=16;response.ook_age_ms=250;response.pulses='350 1050';await tick();
  assert.match(element('details').textContent,/350 1050/);
  response.ook_age_ms=6000;await tick();assert.doesNotMatch(element('details').textContent,/350 1050/);
  response.streaming=false;await tick();assert.match(element('details').textContent,/当前没有实时脉冲/);
  response.streaming=true;element('subAnalysis').value='4';element('subAnalysis').handlers.change();
  await element('form').handlers.submit({preventDefault(){}});await tick();
  assert.equal(response.config.mode,4);assert.equal(response.config.monitor,true);
  assert.match(element('details').textContent,/48 kHz/);
  element('subScan').onclick();assert.equal(element('capturePanel').hidden,false);
  await element('form').handlers.submit({preventDefault(){}});
  assert.equal(response.config.mode,7);assert.equal(response.config.scan_start_hz,314000000);assert.equal(response.config.scan_stop_hz,316000000);
  element('subTab').handlers.click();element('subCapture').onclick();
  await element('form').handlers.submit({preventDefault(){}});
  assert.equal(response.config.mode,9);assert.equal(response.config.frequency_hz,315000000);assert.equal(response.config.monitor,false);
  vm.runInContext('dashboard.disconnected()',context);assert.match(element('mapCaption').textContent,/0 架有坐标/);
  element('captureTab').handlers.click();assert.equal(element('capturePanel').hidden,false);
  element('captureKind').value='7';element('captureKind').handlers.change();
  element('captureStart').value='89';element('captureStart').handlers.input();
  element('captureStop').value='90';element('captureStop').handlers.input();
  await element('form').handlers.submit({preventDefault(){}});
  assert.equal(response.config.mode,7);assert.equal(response.config.enabled,true);assert.equal(response.config.decode,false);
  assert.equal(response.config.scan_start_hz,89000000);assert.equal(response.config.scan_stop_hz,90000000);
  response.view_generation=50;response.scan_coverage=60;response.scan_step=4;response.scan_steps=7;
  response.scan_sweeps=1;response.scan_peak_hz=89800000;response.sequence++;await tick();
  const clears=canvasClears.waterfall;response.generation++;response.sequence++;await tick();
  assert.equal(canvasClears.waterfall,clears,'A scan hop must retain the full-band plot');
  await element('scanPause').onclick();assert.equal(response.config.enabled,false);
  response.streaming=false;await tick();assert.match(element('captureSummary').textContent,/已暂停/);
  await element('scanPeak').onclick();assert.equal(response.config.mode,9);assert.equal(response.config.frequency_hz,89800000);
  response.record_ready=response.record_saved=true;response.record_bytes=5760000;response.record_gaps=0;response.store_available=true;
  response.sd_files=[{name:'IQ000001.IQ',bytes:5760000,frequency_hz:106200000}];++response.sd_catalog_generation;
  await tick();assert.match(element('captureSummary').textContent,/10\.000/);assert.match(element('captureSummary').textContent,/已保存到 SD 卡/);
  assert.equal(element('iqDataLink').attributes['aria-disabled'],'false');
  assert.equal(element('iqDataLink').href,'/api/sdr/record?file=IQ000001.IQ');
  element('captureKind').value='-1';element('captureKind').handlers.change();await tick();
  assert.match(element('captureSummary').textContent,/SD 文件重放/);
  element('sdMode').value='3';await element('sdPlay').onclick();await tick();
  const play=JSON.parse(calls.findLast(c=>c.options.method==='POST').options.body);
  assert.deepEqual(play,{action:'play',file:'IQ000001.IQ',mode:3});
  assert.match(element('live').textContent,/SD 重放/);assert.equal(element('capturePanel').hidden,false);
  assert.equal(element('recordStart').disabled,true);assert.equal(element('sdRefresh').disabled,true);
  assert.equal(element('sdFiles').disabled,true);assert.equal(element('sdStop').disabled,false);
  await element('sdStop').onclick();await tick();assert.equal(element('sdStop').disabled,true);
  assert.match(element('live').textContent,/SD 重放/,'Stopped recorded plot must not claim live RF');
  rejectPost=true;await element('sdPlay').onclick();assert.match(element('message').textContent,/HF needs Direct Q/);rejectPost=false;
  response.sd_mounted=false;await tick();assert.equal(element('sdPlay').disabled,true);assert.equal(element('recordStart').disabled,true);
  response.config.enabled=false;response.config.monitor=false;await tick();
  element('radioTab').handlers.click();
  assert.equal(element('mode').selectedIndex,3);assert.equal(element('monitor').checked,true);
  assert.equal(element('enabled').checked,true);
  await element('form').handlers.submit({preventDefault(){}});await tick();
  assert.equal(response.replay_source,false);assert.equal(response.config.monitor,true);
  element('monitor').checked=false;await element('form').handlers.submit({preventDefault(){}});
  assert.equal(response.config.monitor,false,'Applying explicit mute must not auto-unmute');
  for(const mode of [2,4]){element('mode').selectedIndex=mode;element('mode').handlers.change();
    assert.equal(element('monitor').checked,true);assert.equal(element('enabled').checked,true);}
  console.log('PASS audio mode web: IQ/replay -> Radio restores WFM output, AM/NFM defaults, explicit mute');
  console.log('PASS SD web: file list/download, long capture, replay mode, recorded-source label, stop, no-card gates, errors');
  console.log('PASS IQ/Scan web: scan settings, progress, hop retains plot, pause, inspect peak');
  console.log('PASS Sub-GHz web: enable real receiver, live/wait/stale bursts, NFM, preset-preserving IQ/scan');
  console.log('PASS dashboard: offline map, structured aircraft, no-position row, selection, empty/disconnect, CN preset POST');
  console.log('PASS SDR web: raw RX cursor, reboot reset, ADS-B preset, configuration POST, errors and Codec2 status');
})().catch(error=>{console.error(error);process.exitCode=1;});
