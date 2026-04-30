#!/usr/bin/env python3
"""Extract WriteMemory commands for a specific memory ID from a dump.
Shows offset, size, and data hash for each write. Optionally saves raw data.

Usage: python extract_writemem.py <dump.bin> --mem <id> [--save-dir <dir>] [--max-batches N]
"""
import struct, sys, hashlib, os

def u32(d, o): return struct.unpack_from('<I', d, o)[0]
def u64(d, o): return struct.unpack_from('<Q', d, o)[0]

def main():
    args = sys.argv[1:]
    if len(args) < 3 or '--mem' not in args:
        print("Usage: extract_writemem.py <dump.bin> --mem <id> [--save-dir <dir>] [--max-batches N]")
        sys.exit(1)

    dump_path = args[0]
    mem_id = int(args[args.index('--mem') + 1])
    save_dir = args[args.index('--save-dir') + 1] if '--save-dir' in args else None
    max_batches = int(args[args.index('--max-batches') + 1]) if '--max-batches' in args else 30

    if save_dir:
        os.makedirs(save_dir, exist_ok=True)

    data = open(dump_path, 'rb').read()
    off = 0
    batch_idx = 0
    write_idx = 0
    total_bytes = 0
    # Track memory coverage: which byte ranges have been written
    coverage = []  # list of (offset, size, batch, data_hash)

    while off + 4 <= len(data) and batch_idx < max_batches:
        batch_size = u32(data, off); off += 4
        if batch_size == 0 or off + batch_size > len(data):
            break
        batch_end = off + batch_size
        pos = off

        while pos + 8 <= batch_end:
            cmd_type = u32(data, pos)
            cmd_size = u32(data, pos + 4)
            if cmd_size < 8 or pos + cmd_size > batch_end:
                break
            if cmd_type == 0x1FFFF:  # EndOfStream
                break

            if cmd_type == 0x10003:  # BRIDGE_WriteMemory
                payload = data[pos+8 : pos+cmd_size]
                if len(payload) >= 20:
                    wm_mem = u64(payload, 0)
                    wm_off = u64(payload, 8)
                    wm_sz = u32(payload, 16)
                    if wm_mem == mem_id:
                        wm_data = payload[20:20+wm_sz]
                        h = hashlib.md5(wm_data).hexdigest()[:12]
                        # Check for zero/pattern content
                        nonzero = sum(1 for b in wm_data[:1024] if b != 0)
                        pct_nonzero = nonzero * 100 // min(len(wm_data), 1024)
                        print(f"  batch={batch_idx:3d} write#{write_idx:3d}: off={wm_off:10d} sz={wm_sz:8d} hash={h} nonzero={pct_nonzero}%")
                        coverage.append((wm_off, wm_sz, batch_idx, h))
                        total_bytes += wm_sz
                        if save_dir:
                            fn = os.path.join(save_dir, f"wm_{write_idx:04d}_b{batch_idx}_off{wm_off}_sz{wm_sz}.bin")
                            open(fn, 'wb').write(wm_data)
                        write_idx += 1
            pos += cmd_size
        batch_idx += 1
        off = batch_end

    print(f"\n=== Summary for mem={mem_id} ===")
    print(f"  Total writes: {write_idx}")
    print(f"  Total bytes:  {total_bytes:,}")

    # Check for overlapping writes
    coverage.sort()
    overlaps = 0
    for i in range(1, len(coverage)):
        prev_end = coverage[i-1][0] + coverage[i-1][1]
        curr_start = coverage[i][0]
        if curr_start < prev_end:
            overlaps += 1
            if overlaps <= 5:
                print(f"  OVERLAP: [{coverage[i-1][0]}..{prev_end}) vs [{curr_start}..{curr_start+coverage[i][1]})")
    if overlaps > 5:
        print(f"  ... and {overlaps-5} more overlaps")
    print(f"  Overlapping writes: {overlaps}")

    # Byte coverage
    if coverage:
        max_off = max(o + s for o, s, _, _ in coverage)
        print(f"  Address range: [0 .. {max_off:,})")
        # Unique bytes covered (merge intervals)
        merged = []
        for o, s, _, _ in coverage:
            if merged and o <= merged[-1][1]:
                merged[-1] = (merged[-1][0], max(merged[-1][1], o + s))
            else:
                merged.append((o, o + s))
        unique_bytes = sum(e - s for s, e in merged)
        print(f"  Unique bytes covered: {unique_bytes:,}")

if __name__ == '__main__':
    main()
