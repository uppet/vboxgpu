"""Decode all CmdBeginRendering commands from a VBox GPU Bridge command stream dump.
Shows color/depth/stencil attachment parameters including loadOp, storeOp, clearValue.

Usage: python decode_begin_rendering.py <dump.bin> [--max N]
"""
import struct, sys, argparse

LOAD_OP = {0: "LOAD", 1: "CLEAR", 2: "DONT_CARE"}
STORE_OP = {0: "STORE", 1: "DONT_CARE"}
CMD_BEGIN_RENDERING = 0x1000

def decode_begin_rendering(data, off):
    p = off + 8  # skip header
    cb = struct.unpack_from('<Q', data, p)[0]; p += 8
    ax, ay, aw, ah = struct.unpack_from('<IIII', data, p); p += 16
    lo, so = struct.unpack_from('<II', data, p); p += 8
    cr, cg, cb_, ca = struct.unpack_from('<ffff', data, p); p += 16
    iv = struct.unpack_from('<Q', data, p)[0]; p += 8

    result = {
        'cb': cb, 'area': (ax, ay, aw, ah),
        'color_view': iv, 'color_load': lo, 'color_store': so,
        'color_clear': (cr, cg, cb_, ca),
    }

    end = off + struct.unpack_from('<I', data, off + 4)[0]

    # Depth
    if p + 4 <= end:
        hd = struct.unpack_from('<I', data, p)[0]; p += 4
        result['has_depth'] = hd
        if hd and p + 20 <= end:
            dv = struct.unpack_from('<Q', data, p)[0]; p += 8
            dlo, dso = struct.unpack_from('<II', data, p); p += 8
            cd = struct.unpack_from('<f', data, p)[0]; p += 4
            result['depth_view'] = dv
            result['depth_load'] = dlo
            result['depth_store'] = dso
            result['clear_depth'] = cd

    # Stencil
    if p + 4 <= end:
        hs = struct.unpack_from('<I', data, p)[0]; p += 4
        result['has_stencil'] = hs
        if hs and p + 20 <= end:
            sv = struct.unpack_from('<Q', data, p)[0]; p += 8
            slo, sso = struct.unpack_from('<II', data, p); p += 8
            cs = struct.unpack_from('<I', data, p)[0]; p += 4
            result['stencil_view'] = sv
            result['stencil_load'] = slo
            result['stencil_store'] = sso
            result['clear_stencil'] = cs

    return result

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dump', help='Command stream dump file')
    parser.add_argument('--max', type=int, default=10, help='Max commands to show')
    args = parser.parse_args()

    dump = open(args.dump, 'rb').read()
    count = 0
    pos = 0
    batch_num = 0
    while pos + 4 <= len(dump) and count < args.max:
        batch_size = struct.unpack_from('<I', dump, pos)[0]; pos += 4
        if batch_size == 0 or pos + batch_size > len(dump):
            break
        data = dump[pos:pos + batch_size]
        pos += batch_size
        batch_num += 1

        bp = 0
        while bp + 8 <= len(data) and count < args.max:
            cmd = struct.unpack_from('<I', data, bp)[0]
            sz = struct.unpack_from('<I', data, bp + 4)[0]
            if sz < 8:
                break
            if cmd == CMD_BEGIN_RENDERING:
                r = decode_begin_rendering(data, bp)
                target = "swapchain" if r['color_view'] == 0 else f"view={r['color_view']}"
                print(f"[batch {batch_num} @{bp}] BeginRendering cb=0x{r['cb']:x} "
                      f"area={r['area'][2]}x{r['area'][3]} [{target}]")
                print(f"  color:   load={LOAD_OP.get(r['color_load'], r['color_load'])} "
                      f"store={STORE_OP.get(r['color_store'], r['color_store'])} "
                      f"clear=({r['color_clear'][0]:.2f},{r['color_clear'][1]:.2f},"
                      f"{r['color_clear'][2]:.2f},{r['color_clear'][3]:.2f})")
                if r.get('has_depth'):
                    print(f"  depth:   view={r.get('depth_view',0)} "
                          f"load={LOAD_OP.get(r.get('depth_load',0), r.get('depth_load',0))} "
                          f"store={STORE_OP.get(r.get('depth_store',0), r.get('depth_store',0))} "
                          f"clearDepth={r.get('clear_depth',0):.2f}")
                else:
                    print(f"  depth:   none")
                if r.get('has_stencil'):
                    print(f"  stencil: view={r.get('stencil_view',0)} "
                          f"load={LOAD_OP.get(r.get('stencil_load',0), r.get('stencil_load',0))} "
                          f"store={STORE_OP.get(r.get('stencil_store',0), r.get('stencil_store',0))} "
                          f"clearStencil={r.get('clear_stencil',0)}")
                else:
                    print(f"  stencil: none")
                count += 1
            if cmd == 0x1FFFF:
                break
            bp += sz

    print(f"\nTotal: {count} BeginRendering in {batch_num} batches")

if __name__ == '__main__':
    main()
