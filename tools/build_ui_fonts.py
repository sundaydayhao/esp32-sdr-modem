"""Generate the UI catalog and OFL Noto Sans SC subsets used by production LVGL.

Install: npm install --prefix build/font-tools lv_font_conv@1.5.3
Supply an OFL Noto Sans SC regular font with --font. Never uses proprietary fonts.
"""
import argparse
import json
import pathlib
import re
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--font', default='C:/Windows/Fonts/Noto Sans SC (TrueType).otf')
a = p.parse_args()
translations = json.loads((ROOT/'main/ui_zh.json').read_text(encoding='utf-8'))
spec = re.compile(r'%(?:[-+ #0]*\d*(?:\.\d+)?(?:ll|l|z)?[diuoxXfFeEgGcs%])')
for source, target in translations.items():
    assert spec.findall(source) == spec.findall(target), (source, target)
quote = lambda v: json.dumps(v, ensure_ascii=False)
inc = '/* Generated from ui_zh.json by tools/build_ui_fonts.py. */\n'
inc += 'static const char *const ui_strings[][2]={\n'
inc += ''.join('    {'+quote(k)+','+quote(v)+'},\n' for k,v in translations.items())+'};\n'
(ROOT/'main/ui_strings.inc').write_text(inc,encoding='utf-8')
chars = set(''.join(translations.values()))
for name in ['sdr_display.c','display.c','ui_theme.c']:
    chars.update((ROOT/'main'/name).read_text(encoding='utf-8'))
for name in ['sdr_map_places.json']:
    path=ROOT/'main'/name
    if path.exists():chars.update(path.read_text(encoding='utf-8'))
chars = ''.join(sorted(c for c in chars if ord(c)>126))
converter=ROOT/'build/font-tools/node_modules/lv_font_conv/lv_font_conv.js'
for size in [14,18,26]:
    subprocess.run(['node',str(converter),'--font',a.font,'--range','0x20-0x7e',
        '--symbols',chars,'--size',str(size),'--bpp','2','--format','lvgl',
        '--lv-font-name',f'ui_font_{size}','--lv-fallback',f'lv_font_montserrat_{size}',
        '--no-compress','--output',str(ROOT/f'main/ui_font_{size}.c')],check=True)
    output=ROOT/f'main/ui_font_{size}.c'
    output.write_text(output.read_text(encoding='utf-8').replace('"lvgl/lvgl.h"','"lvgl.h"'),encoding='utf-8')
print(f'Generated {len(translations)} translations and {len(chars)} non-ASCII glyphs at 14/18/26 px.')
