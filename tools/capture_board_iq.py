"""Capture real RTL-SDR CU8 from the P4 and export a CRC-verified SigMF pair.

Uses the same SDR service as LCD/Web. No synthetic RF is generated.
"""
import argparse
import json
import re
import time
import zlib
from pathlib import Path
import serial

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port',required=True)
    p.add_argument('--frequency',type=int,default=433920000)
    p.add_argument('--label',required=True)
    p.add_argument('--seconds',type=float,default=3,help='Continuous SD recording duration, 1..120 seconds')
    args=p.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]+',args.label):p.error('invalid label')
    if not 1<=args.seconds<=120:p.error('seconds must be 1..120')
    base=Path(__file__).resolve().parents[1]/'build'/args.label
    base.parent.mkdir(parents=True,exist_ok=True)
    if any(base.with_suffix(ext).exists() for ext in ('.json','.raw','.sigmf-data','.sigmf-meta')):
        p.error('choose a new label')
    ser=serial.Serial(port=None,baudrate=115200,timeout=.1,write_timeout=2)
    ser.dtr=ser.rts=False;ser.port=args.port;ser.open();raw=bytearray()
    def collect(seconds):
        deadline=time.monotonic()+seconds
        while time.monotonic()<deadline:raw.extend(ser.read(8192))
    def command(text,wait=.7):
        ser.write((text+'\n').encode());collect(wait)
    def state():
        command('SDR STATUS')
        rows=re.findall(rb'\[sdr\] (\{[^\r\n]+\})',raw)
        if not rows:raise RuntimeError('No SDR status')
        return json.loads(rows[-1])
    try:
        collect(.3);command(f'SDR IQ {args.frequency}',3)
        before=state()
        if not(before['streaming'] and before['active_hz']==args.frequency and before['mode']==9):
            raise RuntimeError(f'No live IQ stream at requested frequency: {before}')
        if not before.get('sd_mounted'):raise RuntimeError('Insert a FAT SD card and run SDR SDREFRESH')
        command('SDR CAPTURE',args.seconds)
        command('SDR STOPCAP')
        deadline=time.monotonic()+20
        after=state()
        while (after['recording'] or after.get('record_saving')) and time.monotonic()<deadline:after=state()
        if not(after['record_ready'] and after['record_saved']
               and after['record_bytes']>=int(args.seconds*.8*576000) and not after['record_gaps']
               and after['record_frequency_hz']==args.frequency):
            raise RuntimeError(f'Incomplete capture: {after}')
        if after['record_id']<=before['record_id']:raise RuntimeError('Capture did not start a new record')
        raw.clear();command('SDR IQDUMP',0)
        deadline=time.monotonic()+max(60,after['record_bytes']/64000+20)
        while time.monotonic()<deadline and b'[iqbin-end]\n' not in raw:collect(.2)
        base.with_suffix('.raw').write_bytes(raw)
        h=re.search(rb'\[iqbin\] bytes=(\d+) crc=([a-f0-9]{8}) frequency=(\d+) rate=(\d+) gaps=(\d+)\n',raw)
        if not h:raise RuntimeError('Missing IQ binary header')
        size,crc,freq,rate,gaps=int(h[1]),int(h[2],16),int(h[3]),int(h[4]),int(h[5])
        iq=raw[h.end():h.end()+size]
        if size!=after['record_bytes'] or len(iq)!=size or zlib.crc32(iq)!=crc or freq!=args.frequency or gaps:
            raise RuntimeError('IQ length / CRC / metadata mismatch')
        if raw[h.end()+size:h.end()+size+12]!=b'[iqbin-end]\n':raise RuntimeError('Missing end marker')
        if len(set(iq))<16:raise RuntimeError('Unexpected near-constant IQ data')
        meta={'global':{'core:datatype':'cu8','core:sample_rate':rate,'core:version':'1.2.0',
                       'core:description':'Physical RTL-SDR capture via ESP32-P4 USB HS; CRC verified during export'},
              'captures':[{'core:sample_start':0,'core:frequency':freq}],'annotations':[]}
        base.with_suffix('.sigmf-data').write_bytes(iq)
        base.with_suffix('.sigmf-meta').write_text(json.dumps(meta,indent=2),encoding='utf-8')
        report={'bytes':size,'crc32':f'{crc:08x}','frequency_hz':freq,'rate':rate,'duration_s':size/(2*rate),
                'sd_file':after['sd_file'],'sd_persisted':bool(after['record_saved']),
                'unique_byte_values':len(set(iq)),'capture_status':after}
        base.with_suffix('.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
        print(json.dumps(report),flush=True)
    finally:ser.close()

if __name__=='__main__':main()
