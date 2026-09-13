"""Exercise production SDR demodulation, ADS-B and RF-to-PSK paths on the host."""
from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parents[1]
os.chdir(root)
os.environ['PATH'] = 'C:/cygwin64/bin;' + os.environ['PATH']
out = root / 'build/host-tools'
out.mkdir(parents=True, exist_ok=True)
exe = out / 'host_sdr_check.exe'
sources = ['tools/host_sdr_check.c', 'main/sdr_types.c', 'main/sdr_demod.c', 'main/adsb.c',
           'main/sdr_audio_stream.c',
           'main/sdr_presets.c', 'main/sdr_map.c',
           'main/sdr_record.c', 'main/sdr_scan.c',
           'main/psk_tx.c', 'main/psk_rx.c', 'main/psk_modes.c', 'main/psk_codec.c',
           'main/audio_decim.c', 'components/psk_rtty/psk_rtty.c']
subprocess.run(['C:/cygwin64/bin/gcc.exe', '-std=gnu11', '-O2', '-g',
                '-Itools/sdr_host_stubs', '-Imain', '-Icomponents/psk_rtty/include',
                *sources, '-lm', '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
