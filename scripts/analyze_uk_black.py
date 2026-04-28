#!/usr/bin/env python3
"""Analyze Ultrakill black-screen issue from dump + logs."""

import struct, sys, os, subprocess

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DUMP = os.path.join(BASE, 'dumps', 'sc_uk_short.bin')
DECODER_LOG = os.path.join(BASE, 'host_err_uk_short.txt')   # host program stdout (decoder messages)
VALIDATION_LOG = os.path.join(BASE, 'host_out_uk_short.txt')  # host program stderr (validation errors)
REPLAY_LOG = os.path.join(BASE, 'host_err_replay_uk_short.txt')
REPLAY_VALIDATION = os.path.join(BASE, 'host_out_replay_uk_short.txt')
FRAMES_DIR = os.path.join(BASE, 'frames', 'uk_short')

# ── 1. Screenshot analysis ──
print("=" * 60)
print("1. SCREENSHOT ANALYSIS")
print("=" * 60)
import glob
frames = sorted(glob.glob(os.path.join(FRAMES_DIR, 'frame_*.bmp')))
total_frames = len(frames)
black = 0
nonblack = 0
for f in frames:
    with open(f, 'rb') as fh:
        fh.seek(54)
        data = fh.read(256)
        if any(b != 0 for b in data):
            nonblack += 1
        else:
            black += 1
print(f"  Total frames: {total_frames}")
print(f"  All-black: {black}")
print(f"  Non-black: {nonblack}")

# ── 2. Validation error summary ──
print("\n" + "=" * 60)
print("2. VALIDATION ERROR SUMMARY (host_out_uk_short.txt)")
print("=" * 60)
err_types = {}
with open(VALIDATION_LOG) as f:
    for line in f:
        if 'Validation Error:' in line:
            lb = line.find('[')
            rb = line.find(']', lb+1) if lb >= 0 else -1
            if lb >= 0 and rb > lb:
                etype = line[lb+1:rb].strip()
            else:
                etype = line.strip()[:80]
            err_types[etype] = err_types.get(etype, 0) + 1

for etype, count in sorted(err_types.items(), key=lambda x: -x[1]):
    print(f"  [{count:5d}x] {etype}")

# ── 3. Unique destroyed buffer handles ──
print("\n" + "=" * 60)
print("3. DESTROYED BUFFER HANDLES (from validation errors)")
print("=" * 60)
destroyed_handles = set()
with open(VALIDATION_LOG) as f:
    for line in f:
        if 'was destroyed' in line and 'VkBuffer' in line:
            idx = line.find('handle = ')
            if idx >= 0:
                rest = line[idx+9:]
                rb = rest.find(',')
                h = rest[:rb].strip() if rb >= 0 else rest.strip()
                destroyed_handles.add(h)
print(f"  Unique destroyed buffer handles: {len(destroyed_handles)}")
for h in sorted(destroyed_handles):
    print(f"    {h}")

# ── 4. Disasm: batch-level summary ──
print("\n" + "=" * 60)
print("4. COMMAND STREAM DISASSEMBLY")
print("=" * 60)
disasm = subprocess.run(
    [sys.executable, os.path.join(BASE, 'scripts', 'disasm_cmdstream.py'), DUMP],
    capture_output=True, text=True
)
lines = disasm.stdout.split('\n')

# Count command types
cmd_counts = {}
for line in lines:
    line = line.strip()
    if line.startswith('['):
        # Command line: [offset] CmdName ...
        parts = line.split(']')
        if len(parts) >= 2:
            rest = parts[1].strip()
            cmd_name = rest.split()[0] if rest else 'Unknown'
            cmd_counts[cmd_name] = cmd_counts.get(cmd_name, 0) + 1

print("  Command counts:")
for name, count in sorted(cmd_counts.items(), key=lambda x: -x[1]):
    print(f"    [{count:5d}] {name}")

# ── 5. Check for DestroyBuffer / FreeMemory in stream ──
print("\n" + "=" * 60)
print("5. KEY FINDINGS")
print("=" * 60)
has_destroy_buf = cmd_counts.get('DestroyBuffer', 0)
has_free_mem = cmd_counts.get('FreeMemory', 0)
has_destroy_img = cmd_counts.get('DestroyImage', 0)
print(f"  DestroyBuffer in stream: {has_destroy_buf}")
print(f"  FreeMemory in stream: {has_free_mem}")
print(f"  DestroyImage in stream: {has_destroy_img}")

# Find batches with CopyBufferToImage and which buffers they use
print("\n  CopyBufferToImage source buffers used:")
src_bufs = {}
for line in lines:
    if 'CmdCopyBufferToImage' in line and 'srcBuf=' in line:
        idx = line.find('srcBuf=')
        rest = line[idx+7:]
        end = rest.find(' ')
        buf_id = rest[:end] if end >= 0 else rest.strip()
        src_bufs[buf_id] = src_bufs.get(buf_id, 0) + 1
for buf_id, count in sorted(src_bufs.items(), key=lambda x: -x[1]):
    print(f"    srcBuf={buf_id}: {count}x")

# Check Unknown commands
unknown_counts = {}
for line in lines:
    if 'Unknown(' in line:
        idx = line.find('Unknown(')
        rest = line[idx+8:]
        end = rest.find(')')
        unk_id = rest[:end] if end >= 0 else ''
        unknown_counts[unk_id] = unknown_counts.get(unk_id, 0) + 1
if unknown_counts:
    print("\n  Unknown command IDs (not in disasm CMD_NAMES):")
    for uid, count in sorted(unknown_counts.items()):
        name_map = {'65541': 'BRIDGE_TimingSeq (0x10005)', '65542': 'BRIDGE_CopyBufToImgInline (0x10006)'}
        resolved = name_map.get(uid, '?')
        print(f"    Unknown({uid}) = {resolved}: {count}x")

# ── 6. Format analysis ──
print("\n" + "=" * 60)
print("6. IMAGE FORMAT ANALYSIS")
print("=" * 60)
fmt_counts = {}
# Check both host_out and replay_out for CreateImage messages
for logfile in [DECODER_LOG, REPLAY_LOG]:
    if not os.path.exists(logfile):
        continue
    with open(logfile) as fh:
        for line in fh:
            if 'CreateImage:' in line and 'fmt=' in line:
                idx = line.find('fmt=')
                rest = line[idx+4:]
                end = rest.find(' ')
                fmt = rest[:end] if end >= 0 else rest.strip()
                fmt_counts[fmt] = fmt_counts.get(fmt, 0) + 1

# Known format mapping
FMT_MAP = {
    '37': 'VK_FORMAT_R8G8B8A8_UNORM',
    '70': 'VK_FORMAT_BC1_RGB_UNORM_BLOCK',
    '100': 'VK_FORMAT_D16_UNORM_S8_UINT',
    '131': 'VK_FORMAT_D32_SFLOAT_S8_UINT',
    '133': 'VK_FORMAT_BC2_UNORM_BLOCK',
    '137': 'VK_FORMAT_BC3_UNORM_BLOCK',
}
for fmt, count in sorted(fmt_counts.items(), key=lambda x: -x[1]):
    known = FMT_MAP.get(fmt, '???')
    print(f"    fmt={fmt} ({known}): {count}x")

# Check the unknown format
for fmt in fmt_counts:
    try:
        val = int(fmt)
        if val > 1000:
            print(f"\n  WARNING: Non-standard format {fmt} (hex={hex(val)})")
            print(f"    This format is NOT a core Vulkan format.")
            print(f"    Images using it may be created but unusable.")
    except:
        pass

# ── 7. Host decoder output: look for CopyBufToImg SKIP ──
print("\n" + "=" * 60)
print("7. DECODER COPY SKIP MESSAGES")
print("=" * 60)
skip_count = 0
with open(DECODER_LOG) as f:
    for line in f:
        if 'CopyBufToImg SKIP' in line:
            skip_count += 1
            if skip_count <= 5:
                print(f"    {line.strip()}")
if skip_count > 5:
    print(f"    ... ({skip_count} total SKIP messages)")
elif skip_count == 0:
    print("    No SKIP messages found (all lookups succeeded)")

# ── 8. Host decoder output: staging buffer reallocs ──
print("\n" + "=" * 60)
print("8. STAGING BUFFER REALLOCS")
print("=" * 60)
realloc_count = 0
with open(DECODER_LOG) as f:
    for line in f:
        if 'StagingBuf REALLOC' in line:
            realloc_count += 1
            if realloc_count <= 5:
                print(f"    {line.strip()}")
if realloc_count > 5:
    print(f"    ... ({realloc_count} total reallocs)")
elif realloc_count == 0:
    print("    No staging buffer reallocs found")
