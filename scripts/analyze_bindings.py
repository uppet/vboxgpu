#!/usr/bin/env python3
"""Analyze BindImageMemory logs from host stderr to find memory overlaps.

Usage: python analyze_bindings.py <host_err.txt> [--mem MEMID] [--verbose]
"""

import sys, re, argparse
from collections import defaultdict

def parse_bindings(path):
    images = {}   # id -> (w, h, fmt, mip, usage)
    bindings = defaultdict(list)  # memId -> [(imgId, off, reqSize)]

    with open(path) as f:
        for line in f:
            m = re.search(r'CreateImage: id=(\d+) (\d+)x(\d+) fmt=(\S+) usage=(\S+) mip=(\d+)', line)
            if m:
                images[int(m.group(1))] = (int(m.group(2)), int(m.group(3)),
                                            m.group(4), int(m.group(6)), m.group(5))
            m = re.search(r'BindImageMemory: img=(\d+) mem=(\d+) off=(\d+) reqSize=(\d+) reqAlign=(\d+)', line)
            if m:
                img, mem, off, sz = int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4))
                bindings[mem].append((img, off, sz))
    return images, bindings

def find_overlaps(bindings):
    """Return list of (memId, imgA, imgB, overlap_bytes) tuples."""
    results = []
    for mem, entries in sorted(bindings.items()):
        entries.sort(key=lambda x: x[1])
        for i in range(len(entries)):
            for j in range(i+1, len(entries)):
                img_a, off_a, sz_a = entries[i]
                img_b, off_b, sz_b = entries[j]
                if off_a + sz_a > off_b:
                    overlap = off_a + sz_a - off_b
                    results.append((mem, img_a, img_b, overlap))
    return results

def estimate_icd_size(w, h, fmt_str, mip):
    """Estimate what ICD would compute for imageMemSizes (mirror icd_vkCreateImage logic)."""
    # fmt string might be decimal or hex
    try:
        fmt = int(fmt_str, 0)
    except:
        fmt = 0

    # Simplified bpp/blockSize lookup matching ICD's formatBpp/formatBlockSize
    # BC formats (1000470001 = VK_FORMAT_BC7_UNORM_BLOCK = 145 decimal... but shown as hex)
    # VK_FORMAT_BC7_UNORM_BLOCK = 145, BC7_SRGB = 146
    # VK_FORMAT_BC1 = 131-132, BC2 = 135-136, BC3 = 137-138, BC4 = 139-140, BC5 = 141-142, BC6H = 143-144
    # fmt=37 = VK_FORMAT_R8G8B8A8_UNORM (bpp=4)
    # fmt=70 = VK_FORMAT_A2B10G10R10_UNORM_PACK32 (bpp=4) ... actually fmt=70 = VK_FORMAT_R16_SFLOAT
    # fmt=129 = VK_FORMAT_D32_SFLOAT
    # fmt=133 = VK_FORMAT_BC1_RGBA_UNORM_BLOCK

    bc_formats = {131, 132, 133, 134}  # BC1: blockSize=8
    bc16_formats = {135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146}  # BC2-BC7: blockSize=16

    # Handle hex format IDs (e.g. 1000470001 = VK_FORMAT_BC7_UNORM from extension)
    if fmt > 1000000000:
        # Extension formats — BC7 etc
        bs = 16
        bw = (w + 3) // 4
        bh = (h + 3) // 4
        sz = bw * bh * bs
    elif fmt in bc_formats:
        bs = 8
        bw = (w + 3) // 4
        bh = (h + 3) // 4
        sz = bw * bh * bs
    elif fmt in bc16_formats:
        bs = 16
        bw = (w + 3) // 4
        bh = (h + 3) // 4
        sz = bw * bh * bs
    else:
        # Uncompressed
        bpp_map = {
            9: 1, 10: 1,        # R8_UNORM, R8_SNORM
            37: 4, 43: 4, 44: 4, # R8G8B8A8_*
            50: 4, 51: 4,       # B8G8R8A8_*
            64: 2,              # R16_UNORM... actually check
            70: 2,              # R16_SFLOAT
            77: 4,              # R16G16_SFLOAT
            97: 8,              # R16G16B16A16_SFLOAT
            100: 4,             # R32_SFLOAT
            103: 8,             # R32G32_SFLOAT
            109: 16,            # R32G32B32A32_SFLOAT
            124: 4,             # D32_SFLOAT
            125: 4,             # S8_UINT... actually 1
            126: 4, 127: 4, 128: 4, 129: 4, 130: 8,  # depth/stencil
        }
        bpp = bpp_map.get(fmt, 4)
        sz = w * h * bpp

    if mip > 1:
        sz = sz * 4 // 3
    sz = sz * 3 // 2  # 50% headroom
    sz = (sz + 4095) & ~4095
    if sz < 4096:
        sz = 4096
    return sz

def main():
    parser = argparse.ArgumentParser(description='Analyze BindImageMemory overlaps')
    parser.add_argument('log', help='Host stderr log file')
    parser.add_argument('--mem', type=int, default=None, help='Filter by memory ID')
    parser.add_argument('--verbose', '-v', action='store_true', help='Show all images per memory')
    args = parser.parse_args()

    images, bindings = parse_bindings(args.log)

    # Show overlaps
    overlaps = find_overlaps(bindings)
    if overlaps:
        print(f"=== OVERLAPS FOUND: {len(overlaps)} ===\n")
        for mem, img_a, img_b, overlap in overlaps:
            info_a = images.get(img_a, ('?','?','?','?','?'))
            info_b = images.get(img_b, ('?','?','?','?','?'))
            # Find offsets
            off_a = off_b = sz_a = sz_b = 0
            for entries in bindings[mem]:
                if entries[0] == img_a: off_a, sz_a = entries[1], entries[2]
                if entries[0] == img_b: off_b, sz_b = entries[1], entries[2]
            icd_est_a = estimate_icd_size(info_a[0], info_a[1], info_a[2], info_a[3]) if info_a[0] != '?' else '?'
            print(f"  mem={mem}: img={img_a} [{off_a}..{off_a+sz_a}) {info_a[0]}x{info_a[1]} fmt={info_a[2]} mip={info_a[3]}")
            print(f"           vs img={img_b} [{off_b}..{off_b+sz_b}) {info_b[0]}x{info_b[1]} fmt={info_b[2]} mip={info_b[3]}")
            print(f"           overlap={overlap} bytes, host_reqSize_A={sz_a}, icd_est_A={icd_est_a}")
            print()
    else:
        print("No overlaps found.")

    # Show memory layout
    mems_to_show = [args.mem] if args.mem is not None else sorted(bindings.keys())
    if args.verbose:
        for mem in mems_to_show:
            entries = sorted(bindings[mem], key=lambda x: x[1])
            print(f"\n=== Memory {mem}: {len(entries)} images ===")
            print(f"{'img':>5} {'offset':>12} {'reqSize':>10} {'end':>12} {'icd_est':>10}  {'size':>10} {'fmt':>14} {'mip':>4}")
            for img, off, sz in entries:
                info = images.get(img, ('?','?','?','?','?'))
                icd = estimate_icd_size(info[0], info[1], info[2], info[3]) if info[0] != '?' else '?'
                gap = ''
                print(f"{img:5d} {off:12d} {sz:10d} {off+sz:12d} {str(icd):>10}  {info[0]:>5}x{info[1]:<5} {info[2]:>14} {info[3]:>4}")

    # Summary
    print(f"\nTotal: {len(images)} images, {len(bindings)} memories, {sum(len(v) for v in bindings.values())} bindings, {len(overlaps)} overlaps")

if __name__ == '__main__':
    main()
