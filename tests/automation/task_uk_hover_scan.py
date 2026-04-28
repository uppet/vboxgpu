"""
task_uk_hover_scan.py — 用 hover 扫描定位 UltraKill Difficulty 菜单按钮
流程：
  1. 启动游戏到 DIFFICULTY 屏
  2. 沿 x/y 轴移动鼠标，每步截图
  3. 观察哪个位置触发 STANDARD 的 hover 高亮
  4. 确认后点击

输出截图到 automation_out/uk/hover/
"""

import time, sys, os, win32con
sys.path.insert(0, os.path.dirname(__file__))

from capture import capture_host_frame
from input import find_hwnd, click_at, move_to, press_escape, press_key, focus_window
from process import HostServer, GuestProcess
from pathlib import Path
import numpy as np

GAME_EXE = Path(r"S:\bld\vboxgpu\tests\UltraKill\ULTRAKILL.exe")
TITLE_SUBSTR = "ULTRAKILL"
OUT_DIR = Path(r"S:\bld\vboxgpu\automation_out\uk\hover")

def log(msg):
    print(f"[SCAN] {msg}", flush=True)

def snap(tag: str):
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    dst = OUT_DIR / f"{tag}.png"
    try:
        img = capture_host_frame(timeout=4.0)
        img.save(str(dst))
        arr = np.array(img)
        log(f"snap [{tag}] mean={arr.mean():.1f}")
        return img
    except Exception as e:
        log(f"snap [{tag}] FAILED: {e}")
        return None

def wait_for_frame(max_wait=20.0):
    deadline = time.monotonic() + max_wait
    while time.monotonic() < deadline:
        try:
            img = capture_host_frame(timeout=3.0)
            if np.array(img).mean() > 0.5:
                return True
        except Exception:
            pass
        time.sleep(1.0)
    return False

def run():
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    with HostServer() as host:
        log("host_server started")
        with GuestProcess(GAME_EXE, extra_args=[
            "-screen-width", "800", "-screen-height", "600", "-screen-fullscreen", "0"
        ]) as guest:
            log("game started...")
            try:
                hwnd = find_hwnd(TITLE_SUBSTR, timeout=40.0)
            except TimeoutError as e:
                log(f"ERROR: {e}"); return
            log(f"HWND={hwnd:#x}")

            log("waiting 30s for load...")
            time.sleep(30.0)
            wait_for_frame()

            # ESC x2 + 等主菜单
            log("ESC 1...")
            press_escape(hwnd); time.sleep(3.0)
            snap("esc1")
            log("ESC 2...")
            press_escape(hwnd); time.sleep(3.0)
            snap("esc2")
            log("waiting for menu frame...")
            wait_for_frame()
            time.sleep(1.0)
            snap("menu")

            # 点 CONTINUE → 进入 DIFFICULTY 屏
            log("clicking CONTINUE (0.50, 0.50)...")
            click_at(hwnd, 0.50, 0.50)
            time.sleep(2.5)
            snap("00_difficulty_base")

            # === Hover 扫描 STANDARD 按钮 ===
            # 根据上次截图，STANDARD 在左侧列，y 约 0.44~0.50
            # 固定 x，沿 y 方向每隔 0.02 扫一遍
            scan_x = 0.163
            log(f"hover-scanning x={scan_x}, y=0.30..0.60 step=0.02")
            for i, cy in enumerate([round(0.30 + j*0.02, 2) for j in range(16)]):
                move_to(hwnd, scan_x, cy)
                time.sleep(0.3)
                snap(f"scan_x{int(scan_x*100):03d}_y{int(cy*100):03d}")

            # 同样沿 x 方向在 STANDARD 估算 y 扫一遍（y≈0.467）
            scan_y = 0.467
            log(f"hover-scanning y={scan_y}, x=0.02..0.40 step=0.02")
            for i, cx in enumerate([round(0.02 + j*0.02, 2) for j in range(20)]):
                move_to(hwnd, cx, scan_y)
                time.sleep(0.3)
                snap(f"scan_y{int(scan_y*100):03d}_x{int(cx*100):03d}")

            log("scan done. inspect automation_out/uk/hover/ for hover highlights.")

if __name__ == "__main__":
    run()
