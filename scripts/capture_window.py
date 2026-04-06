"""Capture Host window content using BitBlt from screen DC (captures Vulkan)."""
import ctypes, struct, sys
from ctypes import wintypes

u32 = ctypes.windll.user32
gdi = ctypes.windll.gdi32

# Make process DPI aware so coordinates are correct
u32.SetProcessDPIAware()

# Find window
hwnd = u32.FindWindowA(b'VBoxGPUBridgeServer', None)
if not hwnd:
    hwnd = u32.FindWindowA(None, b'VBox GPU Bridge - Host Server')
if not hwnd:
    # Try partial match
    EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
    titles = []
    def cb(h, _):
        buf = ctypes.create_string_buffer(256)
        u32.GetWindowTextA(h, buf, 256)
        t = buf.value.decode('utf-8', errors='ignore')
        if t and 'VBox' in t:
            titles.append((h, t))
        return True
    u32.EnumWindows(EnumWindowsProc(cb), 0)
    if titles:
        hwnd = titles[0][0]
        print(f"Found: {titles}")
    else:
        print("Window not found. Visible windows with 'VBox':", titles)
        sys.exit(1)

# Get window screen position
rect = wintypes.RECT()
u32.GetWindowRect(hwnd, ctypes.byref(rect))
# Get client area position
client_rect = wintypes.RECT()
u32.GetClientRect(hwnd, ctypes.byref(client_rect))

# Convert client (0,0) to screen coords
pt = wintypes.POINT(0, 0)
u32.ClientToScreen(hwnd, ctypes.byref(pt))
x, y = pt.x, pt.y
w, h = client_rect.right, client_rect.bottom
print(f"HWND=0x{hwnd:x} screen=({x},{y}) client={w}x{h}")

# BitBlt from SCREEN DC (captures Vulkan/DX content via DWM composition)
hdc_screen = u32.GetDC(0)  # 0 = entire screen
hdc_mem = gdi.CreateCompatibleDC(hdc_screen)
hbm = gdi.CreateCompatibleBitmap(hdc_screen, w, h)
gdi.SelectObject(hdc_mem, hbm)
gdi.BitBlt(hdc_mem, 0, 0, w, h, hdc_screen, x, y, 0x00CC0020)  # SRCCOPY

class BMI(ctypes.Structure):
    _fields_ = [('sz',ctypes.c_uint32),('w',ctypes.c_int32),('h',ctypes.c_int32),
                ('planes',ctypes.c_uint16),('bpp',ctypes.c_uint16),('comp',ctypes.c_uint32),
                ('imgSz',ctypes.c_uint32),('xppm',ctypes.c_int32),('yppm',ctypes.c_int32),
                ('clrUsed',ctypes.c_uint32),('clrImp',ctypes.c_uint32)]
bmi = BMI(40, w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
buf = (ctypes.c_uint8 * (w * h * 4))()
gdi.GetDIBits(hdc_mem, hbm, 0, h, buf, ctypes.byref(bmi), 0)

px = bytes(buf)
nz = sum(1 for i in range(0, len(px), 4) if px[i]|px[i+1]|px[i+2])
ci = (h//2 * w + w//2) * 4
print(f"nz={nz}/{w*h} ({100*nz//(w*h) if w*h else 0}%)")
print(f"center=BGRA({px[ci]},{px[ci+1]},{px[ci+2]},{px[ci+3]})")

# Sample a grid
for sy in range(0, h, h//4):
    row = []
    for sx in range(0, w, w//4):
        off = (sy*w+min(sx,w-1))*4
        row.append(f"({px[off]},{px[off+1]},{px[off+2]})")
    print(f"  y={sy}: {' '.join(row)}")

out = sys.argv[1] if len(sys.argv) > 1 else r"S:\bld\vboxgpu\window_capture.bmp"
with open(out, "wb") as f:
    fh = bytearray(14); fh[0]=66; fh[1]=77
    struct.pack_into('<I',fh,2,14+40+w*h*4); struct.pack_into('<I',fh,10,54)
    f.write(fh)
    dh = bytearray(40); struct.pack_into('<I',dh,0,40); struct.pack_into('<i',dh,4,w); struct.pack_into('<i',dh,8,-h)
    struct.pack_into('<H',dh,12,1); struct.pack_into('<H',dh,14,32)
    f.write(dh); f.write(px)
print(f"Saved {out}")

gdi.DeleteObject(hbm); gdi.DeleteDC(hdc_mem); u32.ReleaseDC(0, hdc_screen)
