#!/usr/bin/env python3
"""Analyze BMP screenshots from the vbox_host_server replay/capture tool.

Usage:
  check_screenshot.py [path ...]       # analyze one or more BMP files
  check_screenshot.py --glob PATTERN   # glob pattern, e.g. 'dbg_frame*.bmp'
  check_screenshot.py --latest [N]     # analyze N most recent dbg_frame*.bmp (default 3)
  check_screenshot.py --batch DUMP     # analyze all DUMP_batch*.bmp files

Options:
  --sample                             # print a 5x5 center grid of pixel values
  --bright-rows R1 R2                  # count bright pixels in row range [R1,R2)
"""
import struct, sys, glob, os, argparse

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def read_bmp(path):
    with open(path, 'rb') as f:
        data = f.read()
    sig = data[:2]
    if sig != b'BM':
        raise ValueError(f'Not a BMP: {path}')
    px_off = struct.unpack_from('<I', data, 10)[0]
    w = struct.unpack_from('<i', data, 18)[0]
    h_raw = struct.unpack_from('<i', data, 22)[0]
    h = abs(h_raw)
    bpp = struct.unpack_from('<H', data, 28)[0]
    return data, px_off, w, h, bpp

def analyze(path, sample=False, bright_rows=None):
    try:
        data, px_off, w, h, bpp = read_bmp(path)
    except Exception as e:
        print(f'{os.path.basename(path)}: ERROR — {e}')
        return

    px = data[px_off:]
    stride = bpp // 8  # bytes per pixel (assume 32-bit BGRA)
    total = w * h

    non_zero = 0
    unique_colors = set()
    bright = 0  # R,G,B all > 200

    for i in range(total):
        b = px[i*stride] if i*stride < len(px) else 0
        g = px[i*stride+1] if i*stride+1 < len(px) else 0
        r = px[i*stride+2] if i*stride+2 < len(px) else 0
        if r or g or b:
            non_zero += 1
        unique_colors.add((r, g, b))
        if r > 200 and g > 200 and b > 200:
            bright += 1

    n_colors = len(unique_colors)
    status = 'OK' if n_colors > 100 else ('BLACK' if non_zero == 0 else 'CORRUPTED/MINIMAL')

    print(f'{os.path.basename(path)}: {w}x{h} {bpp}bpp | '
          f'{n_colors} colors | {non_zero}/{total} non-zero | '
          f'{bright} bright | [{status}]')

    if bright_rows is not None:
        r1, r2 = bright_rows
        row_bright = sum(
            1 for row in range(r1, min(r2, h))
              for col in range(w)
            if (lambda i=row*w+col:
                px[i*stride+2]>200 and px[i*stride+1]>200 and px[i*stride]>200)()
        )
        print(f'  Bright pixels in rows {r1}-{r2}: {row_bright}')

    if sample:
        print(f'  Center 5x5 sample (R,G,B):')
        cx, cy = w//2, h//2
        for row in range(cy-2, cy+3):
            line = '    '
            for col in range(cx-2, cx+3):
                i = row*w+col
                b = px[i*stride]; g = px[i*stride+1]; r = px[i*stride+2]
                line += f'({r:3},{g:3},{b:3}) '
            print(line)

def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('paths', nargs='*', help='BMP file(s) to analyze')
    p.add_argument('--glob', help='glob pattern for BMP files')
    p.add_argument('--latest', nargs='?', const=3, type=int, metavar='N',
                   help='analyze N most recent dbg_frame*.bmp (default 3)')
    p.add_argument('--batch', metavar='DUMP', help='analyze all DUMP_batch*.bmp files')
    p.add_argument('--sample', action='store_true', help='print center 5x5 pixel grid')
    p.add_argument('--bright-rows', nargs=2, type=int, metavar=('R1', 'R2'),
                   help='count bright pixels in row range')
    args = p.parse_args()

    files = list(args.paths)

    if args.glob:
        files += sorted(glob.glob(args.glob))

    if args.latest is not None:
        pattern = os.path.join(BASE, 'dbg_frame*.bmp')
        all_frames = sorted(glob.glob(pattern), key=os.path.getmtime)
        files += all_frames[-args.latest:]

    if args.batch:
        pattern = args.batch + '_batch*.bmp'
        files += sorted(glob.glob(pattern), key=lambda p: int(
            p.split('_batch')[-1].replace('.bmp', '') or '0'))

    if not files:
        # default: latest 3 dbg_frame*.bmp
        pattern = os.path.join(BASE, 'dbg_frame*.bmp')
        files = sorted(glob.glob(pattern), key=os.path.getmtime)[-3:]
        if not files:
            print('No BMP files found. Pass a path or use --glob/--latest/--batch.')
            return

    for path in files:
        analyze(path, sample=args.sample, bright_rows=args.bright_rows)

if __name__ == '__main__':
    main()
