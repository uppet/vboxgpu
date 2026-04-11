#!/usr/bin/env python3
"""Trace first scene render pass in a dump — show commands between BeginRendering(internal) and EndRendering."""
import struct, sys

dump_path = sys.argv[1] if len(sys.argv) > 1 else 'dumps/sc14.bin'
CMD = {18:'QueueSubmit',50:'CreateBuffer',51:'DestroyBuffer',54:'CreateImage',55:'DestroyImage',
       57:'CreateImageView',59:'CreateShaderModule',65:'CreatePipeline',
       68:'CreatePipelineLayout',72:'CreateDSL',74:'CreateDescPool',
       85:'CreateCmdPool',88:'AllocCB',90:'BeginCB',91:'EndCB',
       93:'BindPipeline',94:'SetViewport',95:'SetScissor',
       103:'BindDescSets',104:'BindIndexBuffer',106:'CmdDraw',107:'CmdDrawIndexed',
       117:'UpdateBuffer',132:'PushConstants',
       214:'EndRendering',215:'SetCullMode',216:'SetFrontFace',
       221:'SetDepthTestEnable',222:'SetDepthWriteEnable',223:'SetDepthCompareOp',
       224:'SetDepthBoundsTestEnable',228:'SetDepthBiasEnable',
       0x1000:'BeginRendering',0x1004:'AllocDescSets',0x1005:'UpdateDescSets',
       0x1007:'PushDescSet',0x1008:'Barrier2',0x100D:'BindVertexBuffers',
       0x1010:'CopyBuffer',0x1011:'CopyBufToImg',0x1013:'CopyImage',0x1014:'BlitImage',
       0x10000:'CreateSwapchain',0x10002:'QueuePresent',0x10003:'WriteMemory',0x1FFFF:'EndOfStream'}

with open(dump_path,'rb') as f: data=f.read()
# Find batch with first scene rendering (BeginRendering with non-zero imageViewId)
off=0; batch=0
while off<len(data) and batch<5:
    bsz=struct.unpack_from('<I',data,off)[0]; off+=4
    bd=data[off:off+bsz]; off+=bsz; batch+=1
    p=0; in_scene=False; scene_count=0
    while p+8<=len(bd):
        cmd=struct.unpack_from('<I',bd,p)[0]
        csz=struct.unpack_from('<I',bd,p+4)[0]
        if csz<8 or p+csz>len(bd): break
        name=CMD.get(cmd,f'cmd={cmd}')

        if cmd==0x1000:  # BeginRendering
            cbId=struct.unpack_from('<Q',bd,p+8)[0]
            # Read imageViewId at offset 8+4*8+8 = 48? Let me calc: cb(8)+area(4*4)+load(4)+store(4)+clear(4*4)+ivId(8)
            # = 8+16+4+4+16+8 = 56 bytes into payload
            if csz >= 8+56:
                ivId=struct.unpack_from('<Q',bd,p+8+48)[0]
                if ivId != 0 and scene_count < 3:  # get first 3 scene passes
                    in_scene=True; scene_count+=1
                    print(f"=== Batch {batch} Scene Pass (ivId={ivId}) ===")

        if in_scene:
            extra=''
            if cmd==93 and csz>=28:  # BindPipeline
                bp=struct.unpack_from('<I',bd,p+16)[0]
                pipId=struct.unpack_from('<Q',bd,p+20)[0]
                extra=f' bindPoint={bp} pipeline={pipId}'
            elif cmd==94 and csz>=40:  # SetViewport
                x=struct.unpack_from('<f',bd,p+16)[0]
                y=struct.unpack_from('<f',bd,p+20)[0]
                w=struct.unpack_from('<f',bd,p+24)[0]
                h=struct.unpack_from('<f',bd,p+28)[0]
                extra=f' ({x},{y},{w},{h})'
            elif cmd==107 and csz>=36:  # DrawIndexed
                ic=struct.unpack_from('<I',bd,p+16)[0]
                inst=struct.unpack_from('<I',bd,p+20)[0]
                extra=f' idx={ic} inst={inst}'
            elif cmd==221 and csz>=20:  # SetDepthTestEnable
                extra=f' enable={struct.unpack_from("<I",bd,p+16)[0]}'
            elif cmd==215 and csz>=20:  # SetCullMode
                extra=f' mode={struct.unpack_from("<I",bd,p+16)[0]}'
            elif cmd==223 and csz>=20:  # SetDepthCompareOp
                extra=f' op={struct.unpack_from("<I",bd,p+16)[0]}'

            print(f'  [{p:>10}] {name:20s} size={csz:>6}{extra}')

        if cmd==214 and in_scene:  # EndRendering
            in_scene=False
            print(f"  === End Scene Pass ===\n")
        p+=csz
    pass  # continue to next batch
