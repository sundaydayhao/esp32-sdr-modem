"""Collect real RTL-SDR board telemetry; no simulated RF or automatic pass claim.

Use IDF Python. --step SECONDS:COMMAND schedules service commands; all raw
serial data and parsed snapshots are retained under build/<label>.*.
"""
import argparse
import datetime
import json
import re
import time
from pathlib import Path

import serial
from esptool.reset import HardReset


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port', required=True)
    p.add_argument('--seconds', type=float, default=90)
    p.add_argument('--label', required=True)
    p.add_argument('--reset', action='store_true')
    p.add_argument('--expected-boots', type=int, choices=(0, 1), default=None,
                   help='Allow one already-authorized boot when attaching immediately after flashing')
    p.add_argument('--poll', type=float, default=3)
    p.add_argument('--step', action='append', default=[])
    args = p.parse_args()
    expected_boots = args.expected_boots if args.expected_boots is not None else int(args.reset)
    if not re.fullmatch(r'[A-Za-z0-9_-]+', args.label):
        p.error('label must contain only letters, numbers, underscores and hyphens')
    if args.seconds < 10 or args.poll < 1:
        p.error('seconds >= 10, poll >= 1 required')
    steps = []
    for step in args.step:
        when, command = step.split(':', 1)
        if '\n' in command or '\r' in command:
            p.error('one command per step')
        steps.append((float(when), command))
    steps.sort()
    base = Path(__file__).resolve().parents[1] / 'build' / args.label
    base.parent.mkdir(parents=True, exist_ok=True)
    if any(base.with_suffix(ext).exists() for ext in ('.log', '.json', '.stop')):
        p.error('choose a new label to preserve evidence')
    ser = serial.Serial(port=None, baudrate=115200, timeout=0.15, write_timeout=2)
    ser.dtr = False
    ser.rts = False
    ser.port = args.port
    ser.open()
    started = time.monotonic()
    start_wall = datetime.datetime.now().astimezone().isoformat()
    pending = b''
    snapshots, commands, panics, boots = [], [], [], []
    next_poll, next_progress = (10 if args.reset else 1), 30
    failure = None
    print(f'START {start_wall} port={args.port} reset={args.reset}', flush=True)
    try:
        with base.with_suffix('.log').open('wb') as log:
            if args.reset:
                ser.reset_input_buffer()
                HardReset(ser, uses_usb=True)()
            while time.monotonic() - started < args.seconds and not base.with_suffix('.stop').exists():
                chunk = ser.read(8192)
                elapsed = time.monotonic() - started
                if chunk:
                    log.write(chunk)
                    log.flush()
                    pending += chunk
                    while b'\n' in pending:
                        line, pending = pending.split(b'\n', 1)
                        text = re.sub(r'\x1b\[[0-9;]*m', '', line.decode('utf-8', errors='replace')).rstrip('\r')
                        at = text.find('[sdr] {')
                        if at >= 0:
                            try:
                                data = json.loads(text[at + 6:])
                                data['host_elapsed'] = round(elapsed, 3)
                                snapshots.append(data)
                            except json.JSONDecodeError:
                                print('TELEMETRY PARSE ERROR ' + text, flush=True)
                        if re.search(r'assert failed|Guru Meditation|abort\(\)|Rebooting|CORRUPT HEAP|watchdog got triggered|Brownout', text, re.I):
                            panics.append(text)
                        if 'ESP-ROM:' in text:
                            boots.append(text)
                        if re.search(r'\[sdr(?:-ok|-error)?\]|\[aircraft|rtlsdr:|ELF file SHA256|assert failed|Guru Meditation|Rebooting', text):
                            print(f'[{elapsed:.3f}] {text}', flush=True)
                if steps and elapsed >= steps[0][0]:
                    _, command = steps.pop(0)
                    ser.write((command + '\n').encode())
                    commands.append({'elapsed': round(elapsed, 3), 'command': command})
                    print('COMMAND ' + command, flush=True)
                if elapsed >= next_poll:
                    ser.write(b'SDR STATUS\n')
                    next_poll = elapsed + args.poll
                if elapsed >= next_progress:
                    print(f'PROGRESS {elapsed:.1f}s snapshots={len(snapshots)} panic_lines={len(panics)}', flush=True)
                    next_progress = elapsed + 30
    except (serial.SerialException, OSError) as exc:
        failure = repr(exc)
    finally:
        ser.close()
        summary = {'start': start_wall, 'elapsed': round(time.monotonic() - started, 3),
                   'port': args.port, 'intentional_reset': args.reset, 'serial_error': failure,
                   'expected_boots': expected_boots,
                   'commands': commands, 'pending_steps': steps, 'boot_banners': boots,
                   'panic_lines': panics, 'snapshots': snapshots,
                   'stopped_by_file': base.with_suffix('.stop').exists()}
        base.with_suffix('.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
        print(f'SUMMARY snapshots={len(snapshots)} boots={len(boots)} panic_lines={len(panics)} error={failure}', flush=True)
    return 1 if (failure or panics or not snapshots or steps
                 or len(boots) > expected_boots
                 or base.with_suffix('.stop').exists()) else 0


if __name__ == '__main__':
    raise SystemExit(main())
