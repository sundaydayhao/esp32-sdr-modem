"""Exercise physical line-audio TX/RX, including the default SDR input handoff.

Requires IDF Python (pyserial/esptool). Sends actual audio TX for bounded
intervals and restores RX on exit. No RF reception or audio loopback is implied.
Use --reset to include one intentional USB Serial/JTAG reset before testing.
"""
import argparse
import datetime
import json
import re
import time
from pathlib import Path

import serial
from esptool.reset import HardReset

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--port', required=True)
p.add_argument('--label', required=True)
p.add_argument('--content', choices=('TEXT', 'IMAGE', 'AUDIO'), required=True)
p.add_argument('--cycles', type=int, default=5)
p.add_argument('--reset', action='store_true')
args = p.parse_args()
if not re.fullmatch(r'[a-zA-Z0-9_-]+', args.label) or not 1 <= args.cycles <= 20:
    p.error('use a safe label and 1..20 cycles')
base = Path(__file__).resolve().parents[1] / 'build' / args.label
base.parent.mkdir(parents=True, exist_ok=True)
if any(base.with_suffix(ext).exists() for ext in ('.log', '.json')):
    p.error('choose a new label to preserve previous evidence')
s = serial.Serial(port=None, baudrate=115200, timeout=.1, write_timeout=2)
s.dtr = False
s.rts = False
s.port = args.port
s.open()
start = time.monotonic()
wall_start = datetime.datetime.now().astimezone().isoformat()
raw = bytearray()
completed = []
encoded_samples = []
failure = None
panic = r'Guru Meditation|panic.ed|abort\(\)|assert failed|Stack canary|CORRUPT HEAP|Brownout|watchdog got triggered|Rebooting'
log = base.with_suffix('.log').open('wb')


def read_for(seconds):
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        chunk = s.read(8192)
        raw.extend(chunk)
        log.write(chunk)
        log.flush()


def send(command):
    print(f'{time.monotonic()-start:.1f}s > {command}', flush=True)
    s.write((command + '\n').encode())


def diagnose(role, source, active):
    offset = len(raw)
    send('STATUS')
    read_for(.5)
    send('RXDIAG')
    read_for(1)
    reply = raw[offset:].decode(errors='replace')
    match = re.search(r'\[status\] role=(\w+).*?tx=(\d+)/(\d+) content=(\w+)', reply)
    assert match and match[1] == role and match[4].upper() == args.content, reply
    assert int(match[2]) == active and (active or int(match[3]) == 0), reply
    assert f'SDR input={source}' in reply, reply
    assert not re.search(panic, raw.decode(errors='replace'), re.I), 'panic in serial log'
    print(match[0], flush=True)
    return reply


try:
    if args.reset:
        s.reset_input_buffer()
        HardReset(s, uses_usb=True)()
        read_for(10)
        assert raw.count(b'ESP32-P4 SDR modem started') == 1, 'startup not confirmed'
    send('ROLE RX')
    read_for(7)
    send('CONTENT ' + args.content)
    read_for(1)
    diagnose('RX', 'RTL-SDR', 0)
    for cycle in range(args.cycles):
        send('ROLE TX')
        read_for(3)
        reply = diagnose('TX', 'line-audio', 1)
        if args.content == 'AUDIO':
            count = re.search(r'Codec2 ready=1 .*?encode=(\d+)', reply)
            assert count and int(count[1]) > 0, 'Codec2 did not encode during TX'
            # Counters reset at every content session; compare within one TX.
            read_for(1)
            later = diagnose('TX', 'line-audio', 1)
            advanced = re.search(r'Codec2 ready=1 .*?encode=(\d+)', later)
            assert advanced and int(advanced[1]) > int(count[1]), 'Codec2 encoding stalled'
            encoded_samples.append([int(count[1]), int(advanced[1])])
        send('ROLE RX')
        read_for(7)  # Allows the existing bounded 5-second voice drain.
        diagnose('RX', 'line-audio', 0)
        completed.append(cycle + 1)
except Exception as exc:
    failure = str(exc)
finally:
    try:
        send('ROLE RX')
        read_for(7)
        diagnose('RX', 'line-audio', 0)
    except Exception as exc:
        failure = (failure + '; ' if failure else '') + 'cleanup: ' + str(exc)
    s.close()
    log.close()
    text = re.sub(r'\x1b\[[0-9;]*m', '', raw.decode(errors='replace'))
    summary = {
        'start': wall_start, 'elapsed_seconds': round(time.monotonic()-start, 3),
        'content': args.content, 'port': args.port, 'intentional_reset': args.reset,
        'completed_cycles': completed, 'failure': failure,
        'encoded_samples': encoded_samples,
        'boot_banners': text.count('ESP-ROM:'),
        'panics': re.findall(r'^.*(?:' + panic + r').*$', text, re.M | re.I),
        'handoffs': text.count('TX/PTT switched input from RTL-SDR to line audio'),
        'drain_timeouts': text.count('Voice drain timeout'),
        'heap_samples': re.findall(r'heap internal=\d+ largest=\d+ PSRAM=\d+', text),
    }
    summary['passed'] = (
        failure is None and len(completed) == args.cycles and not summary['panics']
        and not summary['drain_timeouts'] and summary['handoffs'] == 1
        and summary['boot_banners'] == (1 if args.reset else 0)
    )
    base.with_suffix('.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    print('SUMMARY ' + json.dumps(summary), flush=True)
raise SystemExit(0 if summary['passed'] else 1)
