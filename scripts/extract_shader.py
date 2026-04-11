#!/usr/bin/env python3
"""Extract a specific shader module's SPIR-V from dump and save as .spv file."""
import struct, sys

dump_path = sys.argv[1]
shader_id = int(sys.argv[2])
out_path = sys.argv[3] if len(sys.argv) > 3 else f'shader_{shader_id}.spv'

with open(dump_path, 'rb') as f: data = f.read()

off = 0
while off < len(data):
    bsz = struct.unpack_from('<I', data, off)[0]; off += 4
    bd = data[off:off+bsz]; off += bsz
    p = 0
    while p + 8 <= len(bd):
        cmd = struct.unpack_from('<I', bd, p)[0]
        csz = struct.unpack_from('<I', bd, p+4)[0]
        if csz < 8 or p + csz > len(bd): break
        if cmd == 59 and csz >= 28:  # CreateShaderModule
            devId = struct.unpack_from('<Q', bd, p+8)[0]
            modId = struct.unpack_from('<Q', bd, p+16)[0]
            codeSize = struct.unpack_from('<I', bd, p+24)[0]
            if modId == shader_id:
                spirv = bd[p+28:p+28+codeSize]
                with open(out_path, 'wb') as fo:
                    fo.write(spirv)
                print(f'Extracted shader {shader_id}: {codeSize} bytes → {out_path}')
                magic = struct.unpack_from('<I', spirv, 0)[0] if codeSize >= 4 else 0
                print(f'  SPIR-V magic: 0x{magic:08x} {"✓" if magic == 0x07230203 else "✗"}')
                sys.exit(0)
        p += csz

print(f'Shader {shader_id} not found in dump')
