#!/usr/bin/env python3
"""Disassemble a recorded VBox GPU Bridge command stream dump.

Usage: python disasm_cmdstream.py <dump.bin> [--spirv-dis] [--max-batches N]

Dump format: sequence of [u32 batch_size][batch_data...]
Each batch contains: [u32 cmd_type][u32 cmd_size][payload...] repeated until EndOfStream.
"""

import struct, sys, os, subprocess, tempfile

CMD_NAMES = {
    0: "CreateInstance", 1: "DestroyInstance", 2: "EnumPhysDevices",
    6: "GetPhysDevProps", 7: "GetQueueFamilyProps", 8: "GetMemProps",
    11: "CreateDevice", 12: "DestroyDevice", 17: "GetDeviceQueue",
    18: "QueueSubmit", 20: "DeviceWaitIdle",
    21: "AllocMemory", 22: "FreeMemory", 23: "MapMemory", 24: "UnmapMemory",
    33: "CreateFence", 34: "DestroyFence", 35: "ResetFences", 37: "WaitForFences",
    38: "CreateSemaphore", 39: "DestroySemaphore",
    44: "BindBufferMemory", 46: "CreateBuffer", 47: "DestroyBuffer",
    48: "BindImageMemory", 50: "CreateImage", 51: "DestroyImage",
    52: "CreateImageView", 53: "DestroyImageView",
    54: "CreateShaderModule", 55: "DestroyShaderModule",
    56: "CreateDescriptorSetLayout",
    58: "CreatePipelineLayout", 59: "DestroyPipelineLayout",
    61: "CreateGraphicsPipelines", 63: "DestroyPipeline",
    67: "CreateRenderPass", 68: "DestroyRenderPass",
    69: "CreateFramebuffer", 70: "DestroyFramebuffer",
    71: "CreateCommandPool", 72: "DestroyCommandPool",
    73: "AllocCommandBuffers", 75: "BeginCommandBuffer", 76: "EndCommandBuffer",
    78: "CmdBindPipeline", 79: "CmdSetViewport", 80: "CmdSetScissor",
    86: "CmdDraw", 87: "CmdPushConstants",
    94: "CmdBeginRenderPass", 96: "CmdEndRenderPass",
    0x1000: "CmdBeginRendering", 0x1001: "CmdEndRendering",
    0x10000: "BRIDGE_CreateSwapchain", 0x10001: "BRIDGE_AcquireNextImage",
    0x10002: "BRIDGE_QueuePresent", 0x1FFFF: "EndOfStream",
}

def u32(data, off): return struct.unpack_from('<I', data, off)[0]
def u64(data, off): return struct.unpack_from('<Q', data, off)[0]
def f32(data, off): return struct.unpack_from('<f', data, off)[0]

def disasm_payload(cmd, data, off, size, spirv_dis=False, spirv_dir=None):
    """Parse command payload and return human-readable string."""
    end = off + size - 8  # size includes 8-byte header
    p = off  # current read position

    def rd32():
        nonlocal p; v = u32(data, p); p += 4; return v
    def rd64():
        nonlocal p; v = u64(data, p); p += 8; return v
    def rdf():
        nonlocal p; v = f32(data, p); p += 4; return v

    try:
        if cmd == 38:  # CreateSemaphore
            return f"dev={rd64()} id={rd64()}"
        elif cmd == 33:  # CreateFence
            return f"dev={rd64()} id={rd64()} flags={rd32()}"
        elif cmd == 35:  # ResetFences
            return f"dev={rd64()} fence={rd64()}"
        elif cmd == 37:  # WaitForFences
            return f"dev={rd64()} fence={rd64()}"
        elif cmd == 71:  # CreateCommandPool
            return f"dev={rd64()} id={rd64()} family={rd32()}"
        elif cmd == 73:  # AllocCommandBuffers
            return f"dev={rd64()} pool={rd64()} cb={rd64()}"
        elif cmd == 75:  # BeginCommandBuffer
            return f"cb=0x{rd64():x}"
        elif cmd == 76:  # EndCommandBuffer
            return f"cb=0x{rd64():x}"
        elif cmd == 54:  # CreateShaderModule
            dev = rd64(); mid = rd64(); code_size = rd32()
            spirv_data = data[p:p+code_size]
            extra = ""
            if spirv_dis and spirv_dir and code_size >= 20:
                spv_path = os.path.join(spirv_dir, f"shader_{mid}.spv")
                with open(spv_path, 'wb') as f: f.write(spirv_data)
                try:
                    r = subprocess.run(['spirv-dis', spv_path], capture_output=True, text=True, timeout=5)
                    if r.returncode == 0:
                        dis_path = os.path.join(spirv_dir, f"shader_{mid}.spvasm")
                        with open(dis_path, 'w') as f: f.write(r.stdout)
                        extra = f" → saved {dis_path}"
                    else:
                        extra = f" spirv-dis failed: {r.stderr[:100]}"
                except FileNotFoundError:
                    # Try Vulkan SDK path
                    sdk = os.environ.get('VULKAN_SDK', r'C:\VulkanSDK\1.3.216.0')
                    spirv_dis_exe = os.path.join(sdk, 'Bin', 'spirv-dis.exe')
                    try:
                        r = subprocess.run([spirv_dis_exe, spv_path], capture_output=True, text=True, timeout=5)
                        if r.returncode == 0:
                            dis_path = os.path.join(spirv_dir, f"shader_{mid}.spvasm")
                            with open(dis_path, 'w') as f: f.write(r.stdout)
                            extra = f" → saved {dis_path}"
                    except: extra = " (spirv-dis not found)"
                except: extra = " (spirv-dis error)"
            # Check SPIR-V magic
            magic = u32(spirv_data, 0) if code_size >= 4 else 0
            return f"dev={dev} id={mid} codeSize={code_size} magic=0x{magic:08x}{extra}"
        elif cmd == 56:  # CreateDescriptorSetLayout
            dev = rd64(); lid = rd64(); bc = rd32()
            bindings = []
            for i in range(bc):
                b, dt, dc, sf = rd32(), rd32(), rd32(), rd32()
                bindings.append(f"({b}:type={dt},cnt={dc},stage=0x{sf:x})")
            return f"dev={dev} id={lid} bindings=[{', '.join(bindings)}]"
        elif cmd == 58:  # CreatePipelineLayout
            dev = rd64(); lid = rd64()
            slc = rd32()
            sets = [str(rd64()) for _ in range(slc)]
            prc = rd32()
            pushes = []
            for _ in range(prc):
                sf, off, sz = rd32(), rd32(), rd32()
                pushes.append(f"(stage=0x{sf:x},off={off},sz={sz})")
            return f"dev={dev} id={lid} setLayouts=[{','.join(sets)}] pushRanges=[{','.join(pushes)}]"
        elif cmd == 61:  # CreateGraphicsPipelines
            dev = rd64(); pid = rd64(); rp = rd64(); lay = rd64()
            vm = rd64(); fm = rd64(); w = rd32(); h = rd32(); cfmt = rd32()
            dynR = "dynRender" if (rp == 0 and cfmt != 0) else "renderPass"
            return f"dev={dev} id={pid} rp={rp} layout={lay} vert={vm} frag={fm} {w}x{h} colorFmt={cfmt} [{dynR}]"
        elif cmd == 78:  # CmdBindPipeline
            return f"cb=0x{rd64():x} pipeline={rd64()}"
        elif cmd == 79:  # CmdSetViewport
            cb = rd64()
            x,y,w,h,mind,maxd = rdf(),rdf(),rdf(),rdf(),rdf(),rdf()
            return f"cb=0x{cb:x} ({x},{y},{w},{h}) depth=[{mind},{maxd}]"
        elif cmd == 80:  # CmdSetScissor
            cb = rd64()
            x,y = struct.unpack_from('<ii', data, p); p += 8
            w,h = rd32(), rd32()
            return f"cb=0x{cb:x} ({x},{y},{w},{h})"
        elif cmd == 86:  # CmdDraw
            cb = rd64(); vc = rd32(); ic = rd32(); fv = rd32(); fi = rd32()
            return f"cb=0x{cb:x} verts={vc} instances={ic} firstVert={fv} firstInst={fi}"
        elif cmd == 87:  # CmdPushConstants
            cb = rd64(); lay = rd64(); sf = rd32(); off = rd32(); sz = rd32()
            vals = data[p:p+sz]
            hex_preview = vals[:32].hex()
            return f"cb=0x{cb:x} layout={lay} stage=0x{sf:x} off={off} sz={sz} data={hex_preview}..."
        elif cmd == 0x1000:  # CmdBeginRendering
            cb = rd64(); ax = rd32(); ay = rd32(); aw = rd32(); ah = rd32()
            lo = rd32(); so = rd32()
            cr,cg,cb_,ca = rdf(),rdf(),rdf(),rdf()
            load_names = {0:"LOAD", 1:"CLEAR", 2:"DONT_CARE"}
            return f"cb=0x{cb:x} area=({ax},{ay},{aw}x{ah}) load={load_names.get(lo,str(lo))} store={so} clear=({cr:.1f},{cg:.1f},{cb_:.1f},{ca:.1f})"
        elif cmd == 0x1001:  # CmdEndRendering
            return f"cb=0x{rd64():x}"
        elif cmd == 18:  # QueueSubmit
            q = rd64(); cb = rd64(); ws = rd64(); ss = rd64(); f = rd64()
            return f"queue={q} cb=0x{cb:x} waitSem={ws} sigSem={ss} fence={f}"
        elif cmd == 0x10000:  # BRIDGE_CreateSwapchain
            dev = rd64(); sid = rd64(); w = rd32(); h = rd32(); ic = rd32()
            return f"dev={dev} id={sid} {w}x{h} imageCount={ic}"
        elif cmd == 0x10002:  # BRIDGE_QueuePresent
            q = rd64(); sc = rd64(); ws = rd64()
            return f"queue={q} swapchain={sc} waitSem={ws}"
        elif cmd == 94:  # CmdBeginRenderPass
            cb = rd64(); rp = rd64(); fb = rd64(); w = rd32(); h = rd32()
            cr,cg,cb_,ca = rdf(),rdf(),rdf(),rdf()
            return f"cb=0x{cb:x} rp={rp} fb={fb} {w}x{h} clear=({cr:.1f},{cg:.1f},{cb_:.1f},{ca:.1f})"
        elif cmd == 96:  # CmdEndRenderPass
            return f"cb=0x{rd64():x}"
    except:
        pass
    return f"(payload {size-8} bytes)"

def main():
    import argparse
    parser = argparse.ArgumentParser(description='Disassemble VBox GPU Bridge command stream')
    parser.add_argument('dump', help='Path to dump .bin file')
    parser.add_argument('--spirv-dis', action='store_true', help='Disassemble SPIR-V shaders')
    parser.add_argument('--max-batches', type=int, default=0, help='Max batches to show (0=all)')
    parser.add_argument('--output-dir', default=None, help='Output directory for SPIR-V files')
    args = parser.parse_args()

    with open(args.dump, 'rb') as f:
        dump = f.read()

    spirv_dir = args.output_dir
    if args.spirv_dis and not spirv_dir:
        spirv_dir = os.path.splitext(args.dump)[0] + '_shaders'
        os.makedirs(spirv_dir, exist_ok=True)

    pos = 0
    batch_num = 0
    while pos < len(dump):
        if pos + 4 > len(dump): break
        batch_size = u32(dump, pos); pos += 4
        if batch_size == 0 or pos + batch_size > len(dump): break
        batch_data = dump[pos:pos+batch_size]
        pos += batch_size
        batch_num += 1

        if args.max_batches and batch_num > args.max_batches:
            print(f"... ({batch_num-1} batches shown, stopping)")
            break

        print(f"=== Batch {batch_num} ({batch_size} bytes) ===")

        bp = 0
        while bp + 8 <= len(batch_data):
            cmd = u32(batch_data, bp)
            cmd_size = u32(batch_data, bp + 4)
            if cmd_size < 8: break

            name = CMD_NAMES.get(cmd, f"Unknown({cmd})")
            payload_off = bp + 8
            detail = disasm_payload(cmd, batch_data, payload_off, cmd_size,
                                     spirv_dis=args.spirv_dis, spirv_dir=spirv_dir)

            print(f"  [{bp:5d}] {name:30s} size={cmd_size:5d}  {detail}")

            if cmd == 0x1FFFF:  # EndOfStream
                break
            bp += cmd_size

        print()

    print(f"Total: {batch_num} batches, {pos} bytes")
    if spirv_dir and os.path.isdir(spirv_dir):
        spv_files = [f for f in os.listdir(spirv_dir) if f.endswith('.spvasm')]
        if spv_files:
            print(f"\nSPIR-V disassembly saved to {spirv_dir}/:")
            for f in sorted(spv_files):
                print(f"  {f}")

if __name__ == '__main__':
    main()
