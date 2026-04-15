#!/usr/bin/env python3
"""Disassemble a recorded VBox GPU Bridge command stream dump.

Usage: python disasm_cmdstream.py <dump.bin> [--spirv-dis] [--max-batches N]

Dump format: sequence of [u32 batch_size][batch_data...]
Each batch contains: [u32 cmd_type][u32 cmd_size][payload...] repeated until EndOfStream.
"""

import struct, sys, os, subprocess, tempfile

# Updated to match current Venus command IDs from vn_command.h
CMD_NAMES = {
    0: "CreateInstance", 1: "DestroyInstance", 2: "EnumPhysDevices",
    6: "GetPhysDevProps", 7: "GetQueueFamilyProps", 8: "GetMemProps",
    11: "CreateDevice", 12: "DestroyDevice", 17: "GetDeviceQueue",
    18: "QueueSubmit", 20: "DeviceWaitIdle",
    21: "AllocMemory", 22: "FreeMemory", 23: "MapMemory", 24: "UnmapMemory",
    28: "BindBufferMemory", 29: "BindImageMemory",
    33: "CreateFence", 36: "DestroyFence", 37: "ResetFences",
    39: "WaitForFences", 40: "CreateSemaphore", 41: "DestroySemaphore",
    50: "CreateBuffer", 51: "DestroyBuffer",
    54: "CreateImage", 55: "DestroyImage",
    57: "CreateImageView", 58: "DestroyImageView",
    59: "CreateShaderModule", 60: "DestroyShaderModule",
    65: "CreateGraphicsPipelines", 67: "DestroyPipeline",
    68: "CreatePipelineLayout", 69: "DestroyPipelineLayout",
    70: "CreateSampler", 71: "DestroySampler",
    72: "CreateDescriptorSetLayout", 73: "DestroyDescriptorSetLayout",
    74: "CreateDescriptorPool", 75: "DestroyDescriptorPool",
    80: "CreateFramebuffer", 81: "DestroyFramebuffer",
    82: "CreateRenderPass", 83: "DestroyRenderPass",
    85: "CreateCommandPool", 86: "DestroyCommandPool",
    88: "AllocCommandBuffers",
    90: "BeginCommandBuffer", 91: "EndCommandBuffer",
    93: "CmdBindPipeline", 94: "CmdSetViewport", 95: "CmdSetScissor",
    103: "CmdBindDescriptorSets", 104: "CmdBindIndexBuffer",
    106: "CmdDraw", 107: "CmdDrawIndexed",
    117: "CmdUpdateBuffer",
    132: "CmdPushConstants", 133: "CmdBeginRenderPass", 135: "CmdEndRenderPass",
    214: "CmdEndRendering",
    215: "CmdSetCullMode", 216: "CmdSetFrontFace",
    221: "CmdSetDepthTestEnable", 222: "CmdSetDepthWriteEnable",
    223: "CmdSetDepthCompareOp", 224: "CmdSetDepthBoundsTestEnable",
    228: "CmdSetDepthBiasEnable",
    0x1000: "CmdBeginRendering",
    0x1004: "AllocDescriptorSets", 0x1005: "UpdateDescriptorSets",
    0x1007: "CmdPushDescriptorSet",
    0x1008: "CmdPipelineBarrier2", 0x1009: "CmdClearAttachments",
    0x100A: "CmdClearColorImage", 0x100D: "CmdBindVertexBuffers",
    0x1010: "CmdCopyBuffer", 0x1011: "CmdCopyBufferToImage",
    0x1013: "CmdCopyImage",
    0x10000: "BRIDGE_CreateSwapchain", 0x10001: "BRIDGE_AcquireNextImage",
    0x10002: "BRIDGE_QueuePresent", 0x10003: "BRIDGE_WriteMemory",
    0x1FFFF: "EndOfStream",
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
        if cmd == 40:  # CreateSemaphore
            return f"dev={rd64()} id={rd64()}"
        elif cmd == 33:  # CreateFence
            return f"dev={rd64()} id={rd64()} flags={rd32()}"
        elif cmd == 37:  # ResetFences
            return f"dev={rd64()} fence={rd64()}"
        elif cmd == 39:  # WaitForFences
            return f"dev={rd64()} fence={rd64()}"
        elif cmd == 85:  # CreateCommandPool
            return f"dev={rd64()} id={rd64()} family={rd32()}"
        elif cmd == 88:  # AllocCommandBuffers
            return f"dev={rd64()} pool={rd64()} cb={rd64()}"
        elif cmd == 90:  # BeginCommandBuffer
            return f"cb=0x{rd64():x}"
        elif cmd == 91:  # EndCommandBuffer
            return f"cb=0x{rd64():x}"
        elif cmd == 59:  # CreateShaderModule
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
        elif cmd == 72:  # CreateDescriptorSetLayout
            dev = rd64(); lid = rd64(); bc = rd32()
            bindings = []
            for i in range(bc):
                b, dt, dc, sf = rd32(), rd32(), rd32(), rd32()
                bindings.append(f"({b}:type={dt},cnt={dc},stage=0x{sf:x})")
            return f"dev={dev} id={lid} bindings=[{', '.join(bindings)}]"
        elif cmd == 68:  # CreatePipelineLayout
            dev = rd64(); lid = rd64()
            slc = rd32()
            sets = [str(rd64()) for _ in range(slc)]
            prc = rd32()
            pushes = []
            for _ in range(prc):
                sf, off, sz = rd32(), rd32(), rd32()
                pushes.append(f"(stage=0x{sf:x},off={off},sz={sz})")
            return f"dev={dev} id={lid} setLayouts=[{','.join(sets)}] pushRanges=[{','.join(pushes)}]"
        elif cmd == 65:  # CreateGraphicsPipelines
            dev = rd64(); pid = rd64(); rp = rd64(); lay = rd64()
            vm = rd64(); fm = rd64(); w = rd32(); h = rd32(); cfmt = rd32()
            dynR = "dynRender" if (rp == 0 and cfmt != 0) else "renderPass"
            return f"dev={dev} id={pid} rp={rp} layout={lay} vert={vm} frag={fm} {w}x{h} colorFmt={cfmt} [{dynR}]"
        elif cmd == 93:  # CmdBindPipeline
            return f"cb=0x{rd64():x} pipeline={rd64()}"
        elif cmd == 94:  # CmdSetViewport
            cb = rd64()
            x,y,w,h,mind,maxd = rdf(),rdf(),rdf(),rdf(),rdf(),rdf()
            return f"cb=0x{cb:x} ({x},{y},{w},{h}) depth=[{mind},{maxd}]"
        elif cmd == 95:  # CmdSetScissor
            cb = rd64()
            x,y = struct.unpack_from('<ii', data, p); p += 8
            w,h = rd32(), rd32()
            return f"cb=0x{cb:x} ({x},{y},{w},{h})"
        elif cmd == 106:  # CmdDraw
            cb = rd64(); vc = rd32(); ic = rd32(); fv = rd32(); fi = rd32()
            return f"cb=0x{cb:x} verts={vc} instances={ic} firstVert={fv} firstInst={fi}"
        elif cmd == 132:  # CmdPushConstants
            cb = rd64(); lay = rd64(); sf = rd32(); off = rd32(); sz = rd32()
            vals = data[p:p+sz]
            hex_preview = vals[:32].hex()
            return f"cb=0x{cb:x} layout={lay} stage=0x{sf:x} off={off} sz={sz} data={hex_preview}..."
        elif cmd == 0x1000:  # CmdBeginRendering
            cb = rd64(); ax = rd32(); ay = rd32(); aw = rd32(); ah = rd32()
            lo = rd32(); so = rd32()
            cr,cg,cb_,ca = rdf(),rdf(),rdf(),rdf()
            ivId = rd64() if (p + 8 <= end) else 0  # imageViewId (new field)
            load_names = {0:"LOAD", 1:"CLEAR", 2:"DONT_CARE"}
            target = "swapchain" if ivId == 0 else f"view={ivId}"
            return f"cb=0x{cb:x} area=({ax},{ay},{aw}x{ah}) load={load_names.get(lo,str(lo))} [{target}] clear=({cr:.1f},{cg:.1f},{cb_:.1f},{ca:.1f})"
        elif cmd == 214:  # CmdEndRendering
            return f"cb=0x{rd64():x}"
        elif cmd == 18:  # QueueSubmit
            q = rd64(); cb = rd64(); ws = rd64(); ss = rd64(); f = rd64()
            return f"queue={q} cb=0x{cb:x} waitSem={ws} sigSem={ss} fence={f}"
        elif cmd == 0x10000:  # BRIDGE_CreateSwapchain
            dev = rd64(); sid = rd64(); w = rd32(); h = rd32(); ic = rd32()
            return f"dev={dev} id={sid} {w}x{h} imageCount={ic}"
        elif cmd == 0x10003:  # BRIDGE_WriteMemory
            mem = rd64(); off = rd64(); sz = rd32()
            return f"mem={mem} off={off} size={sz}"
        elif cmd == 0x1011:  # CmdCopyBufferToImage
            cb = rd64(); src = rd64(); dst = rd64(); layout = rd32(); rc = rd32()
            regions = []
            for i in range(rc):
                bo = rd32()
                rd32(); rd32()  # bufferRowLength, bufferImageHeight
                rd32(); rd32(); rd32(); rd32()  # imageSubresource
                rd32(); rd32(); rd32()  # imageOffset
                w = rd32(); h = rd32(); d = rd32()  # imageExtent
                regions.append(f"bufOff={bo} {w}x{h}")
            return f"cb=0x{cb:x} srcBuf={src} dstImg={dst} regions=[{', '.join(regions)}]"
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
