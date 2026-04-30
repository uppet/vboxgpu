#!/usr/bin/env python3
"""Compare two command stream dumps to find rendering-relevant differences.

Focuses on: CreateImage, CreateBuffer, BindImageMemory, BindBufferMemory,
CreateShaderModule, CreateGraphicsPipelines, CopyBufferToImage, WriteMemory,
UpdateDescriptorSets, BeginRendering.

Usage: python diff_dumps.py <good.bin> <bad.bin> [--max-batches N]
"""
import struct, sys, hashlib

def u32(d, o): return struct.unpack_from('<I', d, o)[0]
def u64(d, o): return struct.unpack_from('<Q', d, o)[0]

CMD_NAMES = {
    21: "AllocMemory", 22: "FreeMemory",
    28: "BindBufferMemory", 29: "BindImageMemory",
    50: "CreateBuffer", 51: "DestroyBuffer",
    54: "CreateImage", 55: "DestroyImage",
    57: "CreateImageView", 59: "CreateShaderModule",
    65: "CreateGraphicsPipelines", 66: "CreateComputePipelines",
    68: "CreatePipelineLayout", 70: "CreateSampler",
    72: "CreateDescriptorSetLayout", 74: "CreateDescriptorPool",
    82: "CreateRenderPass",
    0x1004: "AllocDescriptorSets", 0x1005: "UpdateDescriptorSets",
    0x1011: "CmdCopyBufferToImage",
    0x10003: "BRIDGE_WriteMemory",
    0x10000: "BRIDGE_CreateSwapchain",
    0x1000: "CmdBeginRendering",
    0x10002: "BRIDGE_QueuePresent",
    18: "QueueSubmit",
    0x1FFFF: "EndOfStream",
}

# Commands we want to track for diffing
TRACK_CMDS = {21, 28, 29, 50, 54, 57, 59, 65, 66, 68, 70, 72, 82,
              0x1004, 0x1005, 0x1011, 0x10003, 0x1000, 0x10000}

def parse_batches(path, max_batches=50):
    """Parse dump file, return list of batches. Each batch = list of (cmd, size, payload_hash, detail)."""
    data = open(path, 'rb').read()
    off = 0
    batches = []
    while off + 4 <= len(data) and len(batches) < max_batches:
        batch_size = u32(data, off); off += 4
        if batch_size == 0 or off + batch_size > len(data):
            break
        batch_end = off + batch_size
        cmds = []
        pos = off
        while pos + 8 <= batch_end:
            cmd_type = u32(data, pos)
            cmd_size = u32(data, pos + 4)
            if cmd_size < 8 or pos + cmd_size > batch_end:
                break
            payload = data[pos+8 : pos+cmd_size]
            detail = ""
            # Extract key fields for important commands
            if cmd_type == 54 and len(payload) >= 40:  # CreateImage
                dev = u64(payload, 0); img_id = u64(payload, 8)
                img_type = u32(payload, 16); fmt = u32(payload, 20)
                w = u32(payload, 24); h = u32(payload, 28); d = u32(payload, 32)
                mip = u32(payload, 36); arr = u32(payload, 40)
                detail = f"id={img_id} {w}x{h} fmt={fmt} mip={mip} arr={arr}"
            elif cmd_type == 50 and len(payload) >= 16:  # CreateBuffer
                dev = u64(payload, 0); buf_id = u64(payload, 8)
                detail = f"id={buf_id}"
            elif cmd_type == 59 and len(payload) >= 20:  # CreateShaderModule
                dev = u64(payload, 0); mod_id = u64(payload, 8)
                code_sz = u32(payload, 16)
                code_hash = hashlib.md5(payload[20:20+code_sz]).hexdigest()[:8] if len(payload) >= 20+code_sz else "?"
                detail = f"id={mod_id} code={code_sz}B hash={code_hash}"
            elif cmd_type == 29 and len(payload) >= 24:  # BindImageMemory
                dev = u64(payload, 0); img = u64(payload, 8); mem = u64(payload, 16)
                mem_off = u64(payload, 24) if len(payload) >= 32 else 0
                detail = f"img={img} mem={mem} off={mem_off}"
            elif cmd_type == 28 and len(payload) >= 24:  # BindBufferMemory
                dev = u64(payload, 0); buf = u64(payload, 8); mem = u64(payload, 16)
                mem_off = u64(payload, 24) if len(payload) >= 32 else 0
                detail = f"buf={buf} mem={mem} off={mem_off}"
            elif cmd_type == 21 and len(payload) >= 24:  # AllocMemory
                dev = u64(payload, 0); mem_id = u64(payload, 8)
                alloc_sz = u64(payload, 16); mem_type = u32(payload, 24) if len(payload) >= 28 else -1
                detail = f"id={mem_id} size={alloc_sz} type={mem_type}"
            elif cmd_type == 0x10003 and len(payload) >= 20:  # WriteMemory
                mem_id = u64(payload, 0); offset = u64(payload, 8)
                write_sz = u32(payload, 16)
                data_hash = hashlib.md5(payload[20:20+write_sz]).hexdigest()[:8] if len(payload) >= 20+write_sz else "?"
                detail = f"mem={mem_id} off={offset} sz={write_sz} hash={data_hash}"
            elif cmd_type == 0x1011 and len(payload) >= 8:  # CopyBufferToImage
                detail = f"payload={len(payload)}B"
            elif cmd_type == 0x1005 and len(payload) >= 8:  # UpdateDescriptorSets
                dev = u64(payload, 0)
                n_writes = u32(payload, 8) if len(payload) >= 12 else 0
                detail = f"writes={n_writes}"
            elif cmd_type == 65 and len(payload) >= 16:  # CreateGraphicsPipelines
                dev = u64(payload, 0); pipe_id = u64(payload, 8)
                detail = f"id={pipe_id}"

            phash = hashlib.md5(payload).hexdigest()[:12]
            cmds.append((cmd_type, cmd_size, phash, detail))

            if cmd_type == 0x1FFFF:
                break
            pos += cmd_size
        batches.append(cmds)
        off = batch_end
    return batches

def summarize_batch(cmds):
    """Count commands by type in a batch."""
    counts = {}
    for cmd, sz, _, _ in cmds:
        name = CMD_NAMES.get(cmd, f"0x{cmd:x}")
        counts[name] = counts.get(name, 0) + 1
    return counts

def main():
    if len(sys.argv) < 3:
        print("Usage: diff_dumps.py <good.bin> <bad.bin> [--max-batches N]")
        sys.exit(1)

    good_path, bad_path = sys.argv[1], sys.argv[2]
    max_b = 20
    if '--max-batches' in sys.argv:
        max_b = int(sys.argv[sys.argv.index('--max-batches') + 1])

    print(f"Parsing {good_path}...")
    good = parse_batches(good_path, max_b)
    print(f"  {len(good)} batches")
    print(f"Parsing {bad_path}...")
    bad = parse_batches(bad_path, max_b)
    print(f"  {len(bad)} batches")

    # Compare batch-by-batch
    for i in range(min(len(good), len(bad))):
        g_cmds = good[i]
        b_cmds = bad[i]
        g_summary = summarize_batch(g_cmds)
        b_summary = summarize_batch(b_cmds)

        if g_summary != b_summary:
            print(f"\n=== BATCH {i}: COMMAND COUNT DIFFERS ===")
            all_keys = sorted(set(list(g_summary.keys()) + list(b_summary.keys())))
            for k in all_keys:
                gc = g_summary.get(k, 0)
                bc = b_summary.get(k, 0)
                if gc != bc:
                    print(f"  {k}: good={gc} bad={bc}")

        # Compare tracked commands in order
        g_tracked = [(c, s, h, d) for c, s, h, d in g_cmds if c in TRACK_CMDS]
        b_tracked = [(c, s, h, d) for c, s, h, d in b_cmds if c in TRACK_CMDS]

        diffs = []
        for j in range(min(len(g_tracked), len(b_tracked))):
            gc, gs, gh, gd = g_tracked[j]
            bc, bs, bh, bd = b_tracked[j]
            if gc != bc:
                diffs.append(f"  [{j}] cmd differs: good={CMD_NAMES.get(gc, hex(gc))} bad={CMD_NAMES.get(bc, hex(bc))}")
            elif gh != bh:
                name = CMD_NAMES.get(gc, hex(gc))
                diffs.append(f"  [{j}] {name} PAYLOAD DIFFERS:")
                diffs.append(f"       good: {gd} (hash={gh})")
                diffs.append(f"       bad:  {bd} (hash={bh})")

        if len(g_tracked) != len(b_tracked):
            diffs.append(f"  tracked cmd count: good={len(g_tracked)} bad={len(b_tracked)}")

        if diffs:
            print(f"\n=== BATCH {i}: TRACKED CMD DIFFERENCES ===")
            for d in diffs[:30]:  # limit output
                print(d)
            if len(diffs) > 30:
                print(f"  ... and {len(diffs)-30} more")
        elif g_summary == b_summary:
            # Batch looks identical
            total = sum(g_summary.values())
            print(f"Batch {i}: {total} cmds - identical")

if __name__ == '__main__':
    main()
