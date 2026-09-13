"""Capture actual board FM demodulator PCM over USB, verified by length and CRC.

Requires firmware with SDR serial commands and the SDR PCM capture feed.
This proves the digital PCM path, not the board's analog speaker output.
"""
import argparse
import json
import math
import re
import struct
import time
import wave
import zlib
from pathlib import Path

import serial


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port', required=True)
    p.add_argument('--frequency', type=int, default=106200000)
    p.add_argument('--seconds', type=int, default=6)
    p.add_argument('--label', required=True)
    args = p.parse_args()
    if not 1 <= args.seconds <= 40 or not 88000000 <= args.frequency <= 108000000:
        p.error('seconds must be 1..40 and frequency 88..108 MHz in Hz')
    if not re.fullmatch(r'[A-Za-z0-9_-]+', args.label):
        p.error('invalid label')
    base = Path(__file__).resolve().parents[1] / 'build' / args.label
    if any(base.with_suffix(ext).exists() for ext in ('.wav', '.json', '.raw')):
        p.error('choose a new label')
    ser = serial.Serial(port=None, baudrate=115200, timeout=.1, write_timeout=2)
    ser.dtr = ser.rts = False
    ser.port = args.port
    ser.open()
    raw = bytearray()
    def collect(seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            raw.extend(ser.read(8192))
    def send(command, wait=.6):
        ser.write((command + '\n').encode())
        collect(wait)
    try:
        collect(.5)
        send(f'SDR FM {args.frequency}', 3)
        send('SDR STATUS')
        statuses = re.findall(rb'\[sdr\] (\{[^\r\n]+\})', raw)
        state = json.loads(statuses[-1]) if statuses else {}
        if state.get('streaming') != 1 or state.get('active_hz') != args.frequency or state.get('mode') != 3:
            raise RuntimeError(f'No live FM stream: {state}')
        count = args.seconds * 48000
        send(f'CAP {count}')
        if f'[cap] armed={count}'.encode() not in raw:
            raise RuntimeError('Capture was not armed')
        print(f'Capturing {args.seconds}s of actual {args.frequency} Hz FM PCM', flush=True)
        collect(args.seconds + 1)
        raw.clear()
        send('CAPBIN', 0)
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline and b'[capbin-end]\n' not in raw:
            collect(.2)
        base.with_suffix('.raw').write_bytes(raw)
        header = re.search(rb'\[capbin\] samples=(\d+) crc=([0-9a-f]{8})\n', raw)
        if not header:
            raise RuntimeError('Missing capture header')
        actual = int(header[1])
        expected_crc = int(header[2], 16)
        pcm = raw[header.end():header.end() + actual * 2]
        if actual != count or len(pcm) != count * 2 or zlib.crc32(pcm) != expected_crc:
            raise RuntimeError(f'Capture count/CRC mismatch: samples={actual}/{count}, bytes={len(pcm)}')
        if raw[header.end() + actual * 2:][:13] != b'[capbin-end]\n':
            raise RuntimeError('Missing binary end marker')
        with wave.open(str(base.with_suffix('.wav')), 'wb') as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(48000)
            wav.writeframes(pcm)
        samples = [v[0] for v in struct.iter_unpack('<h', pcm)]
        result = {'source': 'physical RTL-SDR -> board WFM demodulator, before monitor volume',
                  'frequency_hz': args.frequency, 'sample_rate': 48000, 'samples': actual,
                  'crc32': f'{expected_crc:08x}', 'rms': math.sqrt(sum(v*v for v in samples)/actual),
                  'peak': max(abs(v) for v in samples), 'mean': sum(samples)/actual,
                  'clipped_samples': sum(abs(v) >= 32760 for v in samples), 'start_status': state}
        base.with_suffix('.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
        print(json.dumps(result), flush=True)
    finally:
        ser.close()


if __name__ == '__main__':
    main()
