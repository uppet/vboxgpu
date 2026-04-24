"""Capture Client 0 render window and analyze pixels."""
import ctypes, struct, sys
from ctypes import wintypes

u32 = ctypes.windll.user32
gdi = ctypes.windll.gdi32
u32.SetProcessDPIAware()

# Find "Client 0" window
EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
found = []
def cb(h, _):
    if u32.IsWindowVisible(h):
        buf = ctypes.create_string_buffer(256)
        u32.GetWindowTextA(h, buf, 256)
        t = buf.value.decode('ascii', errors='ignore')
        if 'Client' in t:
            found.append((h, t))
    return True
u32.EnumWindows(EnumWindowsProc(cb), 0)
if not found:
    print("ERROR: no 'Client' window found")
    sys.exit(1)
hwnd, title = found[0]
print(f"Window: '{title}'")

cr = wintypes.RECT()
u32.GetClientRect(hwnd, ctypes.byref(cr))
w, h = cr.right, cr.bottom
print(f"client={w}x{h}")

pt = wintypes.POINT(0, 0)
u32.ClientToScreen(hwnd, ctypes.byref(pt))
sdc = u32.GetDC(0)
mdc = gdi.CreateCompatibleDC(sdc)
bmp = gdi.CreateCompatibleBitmap(sdc, w, h)
gdi.SelectObject(mdc, bmp)
gdi.BitBlt(mdc, 0, 0, w, h, sdc, pt.x, pt.y, 0x00CC0020)

class BMI(ctypes.Structure):
    _fields_ = [('sz',ctypes.c_uint32),('w',ctypes.c_int32),('h',ctypes.c_int32),
                ('planes',ctypes.c_uint16),('bpp',ctypes.c_uint16),('comp',ctypes.c_uint32),
                ('imgSz',ctypes.c_uint32),('xppm',ctypes.c_int32),('yppm',ctypes.c_int32),
                ('clrUsed',ctypes.c_uint32),('clrImp',ctypes.c_uint32)]
bmi = BMI(40, w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
buf = (ctypes.c_uint8 * (w * h * 4))()
gdi.GetDIBits(mdc, bmp, 0, h, buf, ctypes.byref(bmi), 0)
px = bytes(buf)

# Stats
nz = sum(1 for i in range(0, len(px), 4) if px[i]|px[i+1]|px[i+2])
red = sum(1 for i in range(0, len(px), 4) if px[i+2] > 100 and px[i+1] < 80 and px[i] < 80)
bg = sum(1 for i in range(0, len(px), 4) if 10 <= px[i+2] <= 30 and px[i+1] < 20 and 20 <= px[i] <= 30)
print(f"nonzero={nz}/{w*h} ({100*nz//(w*h)}%)")
print(f"red_pixels={red} ({100*red//(w*h)}%)")
print(f"bg_pixels={bg} ({100*bg//(w*h)}%)")

# Sample grid 6x6
for row in range(6):
    cols = []
    sy = row * h // 6
    for col in range(6):
        sx = col * w // 6
        off = (sy * w + sx) * 4
        cols.append(f"({px[off+2]:3d},{px[off+1]:3d},{px[off]:3d})")
    print(f"  y={sy:4d}: {' '.join(cols)}")

out = sys.argv[1] if len(sys.argv) > 1 else r"S:\bld\vboxgpu\client_snap.bmp"
with open(out, "wb") as f:
    fh = bytearray(14); fh[0]=66; fh[1]=77
    struct.pack_into('<I',fh,2,14+40+w*h*4); struct.pack_into('<I',fh,10,54)
    f.write(fh)
    dh = bytearray(40); struct.pack_into('<I',dh,0,40); struct.pack_into('<i',dh,4,w); struct.pack_into('<i',dh,8,-h)
    struct.pack_into('<H',dh,12,1); struct.pack_into('<H',dh,14,32)
    f.write(dh); f.write(px)
print(f"Saved {out}")

gdi.DeleteObject(bmp); gdi.DeleteDC(mdc); u32.ReleaseDC(0, sdc)
