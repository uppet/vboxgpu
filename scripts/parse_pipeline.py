#!/usr/bin/env python3
"""Parse a CreateGraphicsPipelines command from dump to show full details including dynamic states."""
import struct, sys

dump_path = sys.argv[1] if len(sys.argv) > 1 else 'dumps/sc14.bin'
target_off = int(sys.argv[2]) if len(sys.argv) > 2 else 20756956  # offset of CreatePipeline in batch 2

with open(dump_path, 'rb') as f: data = f.read()

# Find batch containing the offset
off = 0; batch_data = None
while off < len(data):
    bsz = struct.unpack_from('<I', data, off)[0]; off += 4
    bd = data[off:off+bsz]
    if target_off < len(bd):
        batch_data = bd
        break
    target_off -= bsz
    off += bsz

if not batch_data:
    print("Offset not found")
    sys.exit(1)

p = target_off
cmd = struct.unpack_from('<I', batch_data, p)[0]
csz = struct.unpack_from('<I', batch_data, p+4)[0]
print(f"cmd={cmd} size={csz}")

# Parse CreateGraphicsPipelines (cmd=65)
# Layout: devId(8) pipId(8) rpId(8) layoutId(8) vertModId(8) fragModId(8) vpW(4) vpH(4) colorFmt(4)
# Then: bindingCount(4) [binding...]  attrCount(4) [attr...] depthFmt(4) hasBlend(4) [blend...]
# Then (if dynamic state from ICD): dynStateCount(4) [dynState(4)...]
q = p + 8  # skip header
devId = struct.unpack_from('<Q', batch_data, q)[0]; q += 8
pipId = struct.unpack_from('<Q', batch_data, q)[0]; q += 8
rpId = struct.unpack_from('<Q', batch_data, q)[0]; q += 8
layoutId = struct.unpack_from('<Q', batch_data, q)[0]; q += 8
vertModId = struct.unpack_from('<Q', batch_data, q)[0]; q += 8
fragModId = struct.unpack_from('<Q', batch_data, q)[0]; q += 8
vpW = struct.unpack_from('<I', batch_data, q)[0]; q += 4
vpH = struct.unpack_from('<I', batch_data, q)[0]; q += 4
colorFmt = struct.unpack_from('<I', batch_data, q)[0]; q += 4

print(f"pipeline={pipId} rp={rpId} layout={layoutId}")
print(f"vertMod={vertModId} fragMod={fragModId}")
print(f"viewport={vpW}x{vpH} colorFmt={colorFmt}")

# Vertex bindings
bindCount = struct.unpack_from('<I', batch_data, q)[0]; q += 4
print(f"vertexBindings={bindCount}")
for i in range(bindCount):
    b = struct.unpack_from('<I', batch_data, q)[0]; q += 4
    s = struct.unpack_from('<I', batch_data, q)[0]; q += 4
    r = struct.unpack_from('<I', batch_data, q)[0]; q += 4
    print(f"  binding[{i}]: slot={b} stride={s} rate={r}")

# Vertex attributes
attrCount = struct.unpack_from('<I', batch_data, q)[0]; q += 4
print(f"vertexAttrs={attrCount}")
for i in range(attrCount):
    loc = struct.unpack_from('<I', batch_data, q)[0]; q += 4
    bind = struct.unpack_from('<I', batch_data, q)[0]; q += 4
    fmt = struct.unpack_from('<I', batch_data, q)[0]; q += 4
    off2 = struct.unpack_from('<I', batch_data, q)[0]; q += 4
    print(f"  attr[{i}]: loc={loc} bind={bind} fmt={fmt} off={off2}")

# Depth format
depthFmt = struct.unpack_from('<I', batch_data, q)[0]; q += 4
print(f"depthFmt={depthFmt}")

# Blend
hasBlend = struct.unpack_from('<I', batch_data, q)[0]; q += 4
print(f"hasBlend={hasBlend}")
if hasBlend:
    bEn = struct.unpack_from('<I', batch_data, q)[0]; q += 4
    src = struct.unpack_from('<I', batch_data, q)[0]; q += 4
    dst = struct.unpack_from('<I', batch_data, q)[0]; q += 4
    op = struct.unpack_from('<I', batch_data, q)[0]; q += 4
    srcA = struct.unpack_from('<I', batch_data, q)[0]; q += 4
    dstA = struct.unpack_from('<I', batch_data, q)[0]; q += 4
    opA = struct.unpack_from('<I', batch_data, q)[0]; q += 4
    mask = struct.unpack_from('<I', batch_data, q)[0]; q += 4
    print(f"  blend: en={bEn} src={src} dst={dst} op={op} mask=0x{mask:x}")

# Dynamic states (if remaining bytes)
remaining = (p + csz) - q
print(f"\nRemaining bytes after blend: {remaining}")
if remaining >= 4:
    dynCount = struct.unpack_from('<I', batch_data, q)[0]; q += 4
    DS_NAMES = {0:'VIEWPORT',1:'SCISSOR',2:'LINE_WIDTH',3:'DEPTH_BIAS',4:'BLEND_CONSTANTS',
                5:'DEPTH_BOUNDS',6:'STENCIL_COMPARE_MASK',7:'STENCIL_WRITE_MASK',8:'STENCIL_REFERENCE',
                1000267000:'VIEWPORT_W_COUNT',1000267001:'SCISSOR_W_COUNT',
                1000267002:'BIND_VTX_STRIDE',1000267003:'CULL_MODE',1000267004:'FRONT_FACE',
                1000267005:'PRIMITIVE_TOPOLOGY',1000267006:'DEPTH_TEST_ENABLE',
                1000267007:'DEPTH_WRITE_ENABLE',1000267008:'DEPTH_COMPARE_OP',
                1000267009:'DEPTH_BOUNDS_TEST_ENABLE',1000267010:'STENCIL_TEST_ENABLE',
                1000267011:'STENCIL_OP',1000377000:'RASTERIZER_DISCARD_ENABLE',
                1000377001:'DEPTH_BIAS_ENABLE',1000377002:'PRIMITIVE_RESTART_ENABLE'}
    print(f"dynamicStateCount={dynCount}")
    for i in range(min(dynCount, 30)):
        ds = struct.unpack_from('<I', batch_data, q)[0]; q += 4
        name = DS_NAMES.get(ds, f'UNKNOWN({ds})')
        print(f"  dynState[{i}]: {ds} = {name}")
