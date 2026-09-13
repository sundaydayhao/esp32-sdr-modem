"""Export the headless LVGL framebuffer captures without third-party packages."""
import struct
import sys
import zlib
from pathlib import Path


def chunk(name, payload):
    return (struct.pack('>I', len(payload)) + name + payload
            + struct.pack('>I', zlib.crc32(name + payload)))


output = Path(sys.argv[1])
output.joinpath('README.md').write_text(
    '# LCD 主机预览\n\n'
    '本目录的 lcd-*.png 由生产 LVGL 界面代码在电脑上渲染。'
    '信号、飞机及数字通信数据为测试夹具，不是开发板屏幕照片或真实射频接收结果。'
    '预览图不叠加额外的顶部标识，以便检查实际产品布局。\n', encoding='utf-8')
for path in output.glob('lcd-*.ppm'):
    magic, dimensions, depth, pixels = path.read_bytes().split(b'\n', 3)
    width, height = map(int, dimensions.split())
    assert magic == b'P6' and depth == b'255'
    assert len(pixels) == width * height * 3
    rows = b''.join(b'\0' + pixels[y * width * 3:(y + 1) * width * 3]
                    for y in range(height))
    data = (b'\x89PNG\r\n\x1a\n'
            + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
            + chunk(b'tEXt', b'Description\0Production LVGL host preview; simulated test data, not live RF.')
            + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
    path.with_suffix('.png').write_bytes(data)
