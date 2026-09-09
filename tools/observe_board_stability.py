"""Observe a physical board for resets/panics and responsive serial controls.

Run with ESP-IDF's Python (pyserial/esptool). Optional content cycling stays
in RX; it exercises Codec2 session initialization without transmitting.
"""
import argparse
import datetime
import json
import re
import sys
import time
from pathlib import Path

import serial
from esptool.reset import HardReset

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--port', required=True)
p.add_argument('--seconds', type=float, default=600)
p.add_argument('--label', required=True)
p.add_argument('--reset', action='store_true')
p.add_argument('--exercise-codec', action='store_true')
args = p.parse_args()
if not re.fullmatch(r'[a-zA-Z0-9_-]+', args.label):
    p.error('--label may contain only letters, digits, underscores and hyphens')
if args.seconds < 30:
    p.error('--seconds must be at least 30 to collect two diagnostics')
if args.exercise_codec and args.seconds < 480:
    p.error('--exercise-codec needs at least 480 seconds for all 36 changes')
base = Path(__file__).resolve().parents[1] / 'build' / args.label
base.parent.mkdir(parents=True, exist_ok=True)
if any(base.with_suffix(ext).exists() for ext in ('.log', '.json', '.stop')):
    p.error('choose a new --label to preserve existing observations')
stop = base.with_suffix('.stop')
ser = serial.Serial(port=None, baudrate=115200, timeout=0.2, write_timeout=2)
ser.dtr = False
ser.rts = False
ser.port = args.port
ser.open()
started = time.monotonic()
wall_start = datetime.datetime.now().astimezone().isoformat()
raw = bytearray()
pending = b''
next_diag = started + 10
next_progress = started + 30
serial_error = None
commands_sent = []
exercise = [(40 + 12 * n, ('AUDIO', 'IMAGE', 'TEXT')[n % 3]) for n in range(36)] if args.exercise_codec else []
print(f'START {wall_start} port={args.port} intentional_reset={args.reset}', flush=True)
try:
    with base.with_suffix('.log').open('wb') as log, base.with_suffix('.timestamped.log').open('w', encoding='utf-8') as timed:
        if args.reset:
            ser.reset_input_buffer()
            HardReset(ser, uses_usb=True)()
        while time.monotonic() - started < args.seconds and not stop.exists():
            chunk = ser.read(8192)
            if chunk:
                raw.extend(chunk)
                log.write(chunk)
                log.flush()
                pending += chunk
                while b'\n' in pending:
                    line, pending = pending.split(b'\n', 1)
                    line = line.decode('utf-8', errors='replace').rstrip('\r')
                    text = f'[{time.monotonic()-started:8.3f}s] {line}'
                    timed.write(text + '\n')
                    timed.flush()
                    if not re.match(r'^[0-9a-f]{8}:', line):
                        print(text, flush=True)
            now = time.monotonic()
            if exercise and now - started >= exercise[0][0]:
                _, mode = exercise.pop(0)
                command = f'CONTENT {mode}'
                ser.write((command + '\n').encode())
                commands_sent.append({'elapsed': round(now - started, 3), 'command': command})
                print(f'COMMAND {now-started:.1f}s {command}', flush=True)
            if now >= next_diag:
                ser.write(b'STATUS\nRXDIAG\n')
                next_diag = now + 15
            if now >= next_progress:
                print(f'PROGRESS elapsed={now-started:.1f}s bytes={len(raw)}', flush=True)
                next_progress = now + 30
except (serial.SerialException, OSError) as exc:
    serial_error = repr(exc)
    print(f'SERIAL ERROR {serial_error}', flush=True)
finally:
    ser.close()
    text = re.sub(r'\x1b\[[0-9;]*m', '', raw.decode('utf-8', errors='replace'))
    summary = {
        'start': wall_start, 'end': datetime.datetime.now().astimezone().isoformat(),
        'elapsed_seconds': round(time.monotonic()-started, 3),
        'port': args.port, 'intentional_reset': args.reset, 'serial_error': serial_error,
        'commands_sent': commands_sent,
        'boot_banners': len(re.findall(r'ESP-ROM:', text)),
        'reset_lines': re.findall(r'^.*rst:.*$', text, re.M),
        'app_started': len(re.findall(r'ESP32-P4 SDR modem started', text)),
        'panic_lines': re.findall(r'^.*(?:Guru Meditation|panic.ed|abort\(\)|assert failed|Stack canary|CORRUPT HEAP|Brownout|watchdog got triggered|Rebooting).*$' , text, re.M | re.I),
        'status_responses': len(re.findall(r'\[status\]', text)),
        'content_commands_acknowledged': re.findall(r'serial control command: (CONTENT (?:AUDIO|IMAGE|TEXT))', text),
        'heap_samples': re.findall(r'heap internal=\d+ largest=\d+ PSRAM=\d+', text),
        'bytes': len(raw), 'stopped_by_file': stop.exists(),
    }
    summary['passed'] = (
        not serial_error and not summary['panic_lines'] and not stop.exists()
        and summary['elapsed_seconds'] >= args.seconds
        and summary['boot_banners'] <= (1 if args.reset else 0)
        and (not args.reset or summary['app_started'] == 1)
        and summary['status_responses'] >= 2
        and len(summary['heap_samples']) >= 2
        and (not args.exercise_codec or (
            len(commands_sent) == 36
            and summary['content_commands_acknowledged'] == [item['command'] for item in commands_sent]
        ))
    )
    base.with_suffix('.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    print('SUMMARY ' + json.dumps(summary), flush=True)
sys.exit(0 if summary['passed'] else 1)
