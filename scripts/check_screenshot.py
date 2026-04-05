#!/usr/bin/env python3
"""Analyze a BMP screenshot from the replay tool."""
import struct, sys

def check_bmp(path):
    with open(path, 'rb') as f:
        f.seek(10); off = struct.unpack('<I', f.read(4))[0]
        f.seek(18); w = struct.unpack('<i', f.read(4))[0]; h = abs(struct.unpack('<i', f.read(4))[0])
        row_stride = (w * 4 + 3) & ~3
        f.seek(off)
        found = 0
        for y in range(h):
            row = f.read(row_stride)
            for x in range(w):
                b,g,r,a = row[x*4:(x+1)*4]
                if r > 5 or g > 5 or b > 5:
                    if found < 20:
                        print(f'  ({x},{y}): ({r},{g},{b},{a})')
                    found += 1
        print(f'Total non-black pixels: {found}')
        if found == 0:
            print('IMAGE IS ALL BLACK')
        elif found < 100:
            print('VERY FEW non-black pixels — likely noise')
        else:
            print(f'Rendering OK — {found} colored pixels')

if __name__ == '__main__':
    check_bmp(sys.argv[1] if len(sys.argv) > 1 else 'dumps/replay_screenshot.bmp')
