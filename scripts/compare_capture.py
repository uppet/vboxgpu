import ctypes, struct, time
from ctypes import wintypes

u32 = ctypes.windll.user32
gdi = ctypes.windll.gdi32
u32.SetProcessDPIAware()

hwnd = u32.FindWindowA(None, b'VBox GPU Bridge - Host Server')
print(f'HWND=0x{hwnd:x}')
if not hwnd:
    exit(1)

u32.SetForegroundWindow(hwnd)
u32.BringWindowToTop(hwnd)
time.sleep(0.5)

rect = wintypes.RECT()
u32.GetClientRect(hwnd, ctypes.byref(rect))
w, h = rect.right, rect.bottom
pt = wintypes.POINT(0, 0)
u32.ClientToScreen(hwnd, ctypes.byref(pt))
print(f"client={w}x{h} screen=({pt.x},{pt.y})")

class BMI(ctypes.Structure):
    _fields_ = [('sz',ctypes.c_uint32),('w',ctypes.c_int32),('h',ctypes.c_int32),
                ('planes',ctypes.c_uint16),('bpp',ctypes.c_uint16),('comp',ctypes.c_uint32),
                ('imgSz',ctypes.c_uint32),('xppm',ctypes.c_int32),('yppm',ctypes.c_int32),
                ('clrUsed',ctypes.c_uint32),('clrImp',ctypes.c_uint32)]

def capture_bitblt():
    hdc = u32.GetDC(0)
    hdc_mem = gdi.CreateCompatibleDC(hdc)
    hbm = gdi.CreateCompatibleBitmap(hdc, w, h)
    gdi.SelectObject(hdc_mem, hbm)
    gdi.BitBlt(hdc_mem, 0, 0, w, h, hdc, pt.x, pt.y, 0x00CC0020)
    bmi = BMI(40, w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = (ctypes.c_uint8 * (w*h*4))()
    gdi.GetDIBits(hdc_mem, hbm, 0, h, buf, ctypes.byref(bmi), 0)
    gdi.DeleteObject(hbm); gdi.DeleteDC(hdc_mem); u32.ReleaseDC(0, hdc)
    return bytes(buf)

def capture_printwin():
    hdc = u32.GetDC(hwnd)
    hdc_mem = gdi.CreateCompatibleDC(hdc)
    hbm = gdi.CreateCompatibleBitmap(hdc, w, h)
    gdi.SelectObject(hdc_mem, hbm)
    ret = u32.PrintWindow(hwnd, hdc_mem, 2)
    bmi = BMI(40, w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = (ctypes.c_uint8 * (w*h*4))()
    gdi.GetDIBits(hdc_mem, hbm, 0, h, buf, ctypes.byref(bmi), 0)
    gdi.DeleteObject(hbm); gdi.DeleteDC(hdc_mem); u32.ReleaseDC(hwnd, hdc)
    return bytes(buf), ret

px1 = capture_bitblt()
px2, pw_ret = capture_printwin()

ci = (h//2*w + w//2)*4
nz1 = sum(1 for i in range(0,len(px1),4) if px1[i]|px1[i+1]|px1[i+2])
nz2 = sum(1 for i in range(0,len(px2),4) if px2[i]|px2[i+1]|px2[i+2])
print(f"BitBlt:      nz={nz1}/{w*h} center=BGRA({px1[ci]},{px1[ci+1]},{px1[ci+2]},{px1[ci+3]})")
print(f"PrintWindow: nz={nz2}/{w*h} center=BGRA({px2[ci]},{px2[ci+1]},{px2[ci+2]},{px2[ci+3]}) ret={pw_ret}")

ab1 = all(px1[i]==0 and px1[i+1]==0 and px1[i+2]==0 for i in range(0,min(len(px1),4000),4))
ab2 = all(px2[i]==0 and px2[i+1]==0 and px2[i+2]==0 for i in range(0,min(len(px2),4000),4))
print(f"First1000px black: BitBlt={ab1} PrintWin={ab2}")

for name, px in [('bitblt', px1), ('printwin', px2)]:
    with open(f'S:/bld/vboxgpu/wincap_{name}.bmp','wb') as f:
        fh=bytearray(14);fh[0]=66;fh[1]=77
        struct.pack_into('<I',fh,2,14+40+w*h*4);struct.pack_into('<I',fh,10,54)
        f.write(fh)
        dh=bytearray(40);struct.pack_into('<I',dh,0,40);struct.pack_into('<i',dh,4,w);struct.pack_into('<i',dh,8,-h)
        struct.pack_into('<H',dh,12,1);struct.pack_into('<H',dh,14,32)
        f.write(dh);f.write(px)
print("Saved wincap_bitblt.bmp and wincap_printwin.bmp")
