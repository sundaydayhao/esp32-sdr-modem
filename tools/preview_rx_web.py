"""Local UI fixture only. Exercises the production HTML without a board or Wi-Fi."""
import json
import re
import struct
import argparse
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--sdr',action='store_true');parser.add_argument('--port',type=int,default=8765)
args=parser.parse_args()
rf=dict(boot_id='preview-only',config=dict(enabled=True,decode=False,monitor=False,mode=6,frontend=0,frequency_hz=1090000000,converter_hz=125000000,volume=35,scan_start_hz=433050000,scan_stop_hz=434790000),
    generation=1,sequence=1,streaming=True,usb_status='HOST TEST / SAMPLE DATA',active_frequency_hz=1090000000,sample_rate_hz=2000000,fft=[0]*512,scan=[0]*512,waveform=[0]*64,
    audio_frames=0,audio_level=0,modem_content=0,modem_mode='PSK125R',iq_blocks=1234,iq_drops=0,pcm_drops=0,audio_drops=0,dsp_max_us=900,adsb_messages=427,aircraft='',pulses='',ook_frames=0,record_status='Preview only',record_bytes=0,record_gaps=0)
aircraft=[dict(icao='40621D',callsign='HOST123',has_position=True,latitude=52.2658,longitude=3.9389,altitude_feet=38000,speed_knots=420,heading_degrees=82,age_ms=700,position_age_ms=700),dict(icao='4CA123',callsign='HOST456',has_position=True,latitude=51.35,longitude=4.6,altitude_feet=26000,heading_degrees=150,age_ms=900,position_age_ms=900),dict(icao='ABCDEF',callsign='NOCPR',has_position=False,age_ms=1200)]
presets=[]
for name,usage,analysis,center,start,stop,mode in re.findall(r'\{\s*"([^"\n]*)",\s*"([^"\n]*)",\s*"([^"\n]*)",\s*(\d+),\s*(\d+),\s*(\d+),\s*(SDR_\w+)\s*\}',(ROOT/'main/sdr_presets.c').read_text(encoding='utf-8')):
    presets.append(dict(name=name,usage=usage.replace('\\n','\n'),analysis=analysis.replace('\\n','\n'),center_hz=int(center),start_hz=int(start),stop_hz=int(stop),mode=8 if mode=='SDR_OOK' else 5))
assert len(presets)==5
mode_names = re.findall(r'MODE\([^,]+, "([^"]+)"', (ROOT/'main/psk_modes.c').read_text())
allowed = {9, 10, 11, 12, 20, 21, 25, 26}
state = dict(boot_id='preview-only', content_epoch=1, content_type=0, mode_id=0, tone_hz=2000,
    role='RX', lock_state=3, dsp_load_percent=12.5, received_bytes=0, baud=1230.769,
    carriers=1, information_bps=2461.538, samples_per_second=48000, dsp_max_us=900,
    block_budget_us=5333, deadline_misses=0, read_errors=0, image_rows=63, image_version=1,
    image_bad_rows=0, voice_rate=700, voice_packets=24, voice_frames=144, voice_crc_errors=0,
    encoded_frames=0, decoded_frames=48, concealed_frames=2, queue_drops=0, codec_ready=True, voice_draining=False, rsid_enabled=True, rsid_detections=1, rsid_id=183, rsid_tone_hz=1500,
    last_voice_frame='65 7A 1C F0')
pixels = bytes(int(x,16) for x in re.findall(r'0x([0-9a-fA-F]{2})', (ROOT/'main/modem_image_pixels.inc').read_text()))
assert len(pixels) == 9216
bmp = bytearray(54 + 96*96*3)
struct.pack_into('<2sIHHIIIIHH', bmp, 0, b'BM', len(bmp), 0, 0, 54, 40, 96, 96, 1, 24)
for y in range(96):
    for x in range(96):
        p = pixels[(95-y)*96+x] if 95-y < 63 else 0
        bmp[54+(y*96+x)*3:54+(y*96+x)*3+3] = bytes(((p&3)*85, ((p>>2)&7)*255//7, (p>>5)*255//7))

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_): pass
    def respond(self, data, mime='application/json', code=200):
        if not isinstance(data, (bytes,bytearray)): data = json.dumps(data).encode()
        self.send_response(code)
        for k,v in {'Content-Type':mime, 'Content-Length':str(len(data)), 'Cache-Control':'no-store',
            'X-Boot-Id':state['boot_id'], 'X-Content-Epoch':str(state['content_epoch']),
            'X-Image-Version':str(state['image_version']), 'X-Next-Cursor':'0', 'X-Lost-Bytes':'0'}.items():
            self.send_header(k,v)
        self.end_headers(); self.wfile.write(data)
    def do_GET(self):
        if self.path in ('/','/sdr','/modem'):
            file='sdr.html' if self.path=='/sdr' or self.path=='/' and args.sdr else 'rx.html'
            self.respond((ROOT/'main'/file).read_bytes(), 'text/html; charset=utf-8')
        elif self.path in ('/theme.css','/dashboard.js','/map-data.json'):
            name={'/map-data.json':'map_data.json'}.get(self.path,self.path[1:])
            mime='text/css' if name.endswith('.css') else 'text/javascript' if name.endswith('.js') else 'application/json'
            self.respond((ROOT/'main'/name).read_bytes(),mime)
        elif self.path == '/api/sdr': self.respond(rf)
        elif self.path == '/api/aircraft': self.respond(dict(boot_id='preview-only',aircraft=aircraft if rf['config']['mode']==6 else []))
        elif self.path == '/api/presets': self.respond(dict(region='CN',presets=presets))
        elif self.path == '/api/status': self.respond(dict(state,mode=mode_names[state['mode_id']]))
        elif self.path == '/api/modes': self.respond([dict(id=i,name=n,image_allowed=i in allowed) for i,n in enumerate(mode_names)])
        elif self.path.startswith('/api/rx?'): self.respond(b'', 'application/octet-stream')
        elif self.path == '/api/image': self.respond(bmp, 'image/bmp')
        else: self.respond({},code=404)
    def do_POST(self):
        value = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        if self.path=='/api/sdr':
            rf['config']=value;rf['generation']+=1;rf['active_frequency_hz']=value['frequency_hz'];self.respond(rf);return
        if self.path=='/api/sdr/record':self.respond(dict(queued=True));return
        state.update(content_type=value['content_type'], mode_id=value['mode'],tone_hz=value['tone_hz'])
        state['content_epoch'] += 1
        self.respond({'queued':True},code=202)

print(f'Local UI fixture at http://127.0.0.1:{args.port} (sample data only)',flush=True)
HTTPServer(('127.0.0.1',args.port),Handler).serve_forever()
