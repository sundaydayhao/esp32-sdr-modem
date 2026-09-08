// Run the production browser script against the documented HTTP response
// shapes. No radio hardware or network service is simulated as a live test.
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const path = require('node:path');
const html = fs.readFileSync(path.join(__dirname, '../main/sdr.html'), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const elements = new Map(), calls = [], timers = [];
function element(id) {
  if (!elements.has(id)) elements.set(id, {
    value: '', checked: false, selectedIndex: 0, textContent: '', hidden: false,
    children: [{}, {}, {}], handlers: {}, width: 768, height: 290,
    addEventListener(type, fn) { this.handlers[type] = fn; },
    getContext() { return {
      clearRect(){}, fillRect(){}, beginPath(){}, moveTo(){}, lineTo(){}, stroke(){}, drawImage(){},
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
  voice_decoded:90,voice_crc_errors:0,image_rows:32
};
let rejectPost = false, rxCounter = 0;
const context = vm.createContext({
  document:{getElementById:element}, TextDecoder, Math, Number, Uint8ClampedArray,
  setTimeout(fn){timers.push(fn);},
  async fetch(url, options={}) {
    calls.push({url,options});
    if (options.method==='POST') {
      if (rejectPost) return {ok:false,text:async()=> 'HF needs Direct Q'};
      if (url==='/api/sdr') { response.config=JSON.parse(options.body); ++response.generation; }
    }
    if(url.startsWith('/api/rx?cursor=')) {
      const text=rxCounter++?' next':'SDR test';
      return {ok:true,headers:{get:key=>key==='X-Content-Epoch'?'1':String(rxCounter*8)},
        arrayBuffer:async()=>new TextEncoder().encode(text).buffer};
    }
    return {ok:true,json:async()=>structuredClone(response)};
  }
});
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
  rejectPost=true;await element('form').handlers.submit({preventDefault(){}});
  assert.match(element('message').textContent,/HF needs Direct Q/);rejectPost=false;
  response.config.mode=0;response.config.decode=true;response.modem_content=2;
  await tick();assert.match(element('details').textContent,/Codec2 700C/);
  assert.match(element('details').textContent,/90/);
  response.boot_id='boot-two';response.modem_content=0;rxCounter=0;
  await tick();assert.equal(calls.findLast(c=>c.url.startsWith('/api/rx')).url,'/api/rx?cursor=0');
  console.log('PASS SDR web: raw RX cursor, reboot reset, ADS-B preset, configuration POST, errors and Codec2 status');
})().catch(error=>{console.error(error);process.exitCode=1;});
