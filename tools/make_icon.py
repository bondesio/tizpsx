#!/usr/bin/env python3
"""Create the TizPSX 256px PNG icon using only Python's standard library."""
import struct
import zlib
from pathlib import Path

W = H = 256
pixels = bytearray(W * H * 3)
for y in range(H):
    for x in range(W):
        i = (y * W + x) * 3
        pixels[i:i + 3] = bytes((7, 24, 43))

def rect(x0, y0, x1, y1, color):
    for y in range(y0, y1):
        for x in range(x0, x1):
            i = (y * W + x) * 3
            pixels[i:i + 3] = bytes(color)

# A compact directional pad and face-button cluster frame the PSX wordmark.
blue, cyan, red, green, yellow = (52, 121, 190), (120, 200, 255), (234, 87, 87), (110, 208, 139), (244, 204, 103)
rect(37, 99, 77, 139, blue); rect(48, 87, 66, 151, blue); rect(25, 110, 89, 128, blue)
rect(184, 97, 205, 118, green); rect(211, 120, 232, 141, red)
rect(184, 143, 205, 164, blue); rect(157, 120, 178, 141, yellow)
font = {
    'P': ['11110','10001','10001','11110','10000','10000','10000'],
    'S': ['01111','10000','10000','01110','00001','00001','11110'],
    'X': ['10001','01010','00100','00100','00100','01010','10001'],
}
scale, gap = 16, 8
letters = 'PSX'
total = len(letters) * 5 * scale + (len(letters) - 1) * gap
sx = (W - total) // 2
sy = 62
for pos, letter in enumerate(letters):
    for yy, row in enumerate(font[letter]):
        for xx, bit in enumerate(row):
            if bit == '1':
                x = sx + pos * (5 * scale + gap) + xx * scale
                y = sy + yy * scale
                rect(x, y, x + scale - 2, y + scale - 2, cyan)

def chunk(kind, data):
    return struct.pack('!I', len(data)) + kind + data + struct.pack('!I', zlib.crc32(kind + data) & 0xffffffff)

raw = b''.join(b'\x00' + pixels[y * W * 3:(y + 1) * W * 3] for y in range(H))
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('!2I5B', W, H, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
root = Path(__file__).resolve().parents[1]
for output in (root / 'app/shared/res/icon.png', root / 'launcher/icon.png'):
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(png)
    print(output)
