#!/usr/bin/env python3
"""Compare dump command sequence with Host log to find where they diverge."""
import struct, sys, re

# Parse dump: extract (offset, cmd, size, objId) for Create commands
dump_path = sys.argv[1] if len(sys.argv) > 1 else 'dumps/sc10.bin'
host_log = sys.argv[2] if len(sys.argv) > 2 else 'host_err_sc11.txt'

CMD_NAMES = {50:'CreateBuffer',54:'CreateImage',59:'CreateShaderModule',
             57:'CreateImageView',70:'CreateSampler',72:'CreateDSL',
             74:'CreateDescPool',68:'CreatePipeLayout',65:'CreatePipeline',
             85:'CreateCmdPool',88:'AllocCB',40:'CreateSemaphore',33:'CreateFence',
             82:'CreateRenderPass',80:'CreateFramebuffer'}

with open(dump_path, 'rb') as f:
    data = f.read()

bsz = struct.unpack_from('<I', data, 0)[0]
bd = data[4:4+bsz]

# Extract create commands with their IDs from dump
dump_creates = []
p = 0
while p + 8 <= len(bd):
    cmd = struct.unpack_from('<I', bd, p)[0]
    csz = struct.unpack_from('<I', bd, p+4)[0]
    if csz < 8 or p + csz > len(bd):
        break
    if cmd in CMD_NAMES and csz >= 24:
        devId = struct.unpack_from('<Q', bd, p+8)[0]
        objId = struct.unpack_from('<Q', bd, p+16)[0]
        if objId < 10000:  # reasonable ID
            dump_creates.append((p, cmd, CMD_NAMES[cmd], objId))
    p += csz

# Parse Host log: extract create commands with IDs
host_creates = []
with open(host_log, errors='replace') as f:
    for line in f:
        m = re.search(r'\[Decoder\] (\w+): id=(\d+)', line)
        if m:
            name = m.group(1)
            oid = int(m.group(2))
            host_creates.append((name, oid))

# Compare: find first mismatch
print(f"Dump: {len(dump_creates)} create commands in batch 0")
print(f"Host: {len(host_creates)} create commands logged")
print()

# Match by ID sequence
di = 0
hi = 0
mismatches = 0
while di < len(dump_creates) and hi < len(host_creates) and mismatches < 5:
    d_off, d_cmd, d_name, d_id = dump_creates[di]
    h_name, h_id = host_creates[hi]

    if d_id == h_id:
        if d_name != h_name.replace('AllocMemory','AllocMemory'):
            # Name mismatch but same ID - possible misparse
            if not (d_name == 'CreateImage' and h_name == 'CreateImage') and \
               not (d_name == 'CreateBuffer' and h_name == 'CreateBuffer'):
                print(f"MISMATCH at id={d_id}: dump={d_name}(cmd={d_cmd}) @ off={d_off}, host={h_name}")
                mismatches += 1
        di += 1
        hi += 1
    elif d_id < h_id:
        # Dump has an ID that host skipped
        print(f"DUMP ONLY: {d_name} id={d_id} @ off={d_off}")
        di += 1
        mismatches += 1
    else:
        # Host has an ID that dump doesn't have
        print(f"HOST ONLY: {h_name} id={h_id}")
        hi += 1
        mismatches += 1

if mismatches == 0:
    print("No mismatches found in create commands!")
