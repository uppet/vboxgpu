"""
task_uk_probe_coords.py — 鼠标扫描探测难度屏 UI 坐标

流程：
  1. 启动 host + ULTRAKILL，到达难度选择屏（同 diff_test）
  2. 在 x=0.163 处，从 y=0.38 到 y=0.60 每隔 0.02 移动鼠标
  3. 每个位置停留 0.5s 后截图，记录 mean 值
  4. hover 高亮会使对应 y 位置的截图明显变亮 → 找到 UI 元素精确位置
"""

import time, sys, os
sys.path.insert(0, os.path.dirname(__file__))

from capture import capture_host_frame
from input import find_hwnd, click_at, press_escape, focus_window, glide_to
from process import HostServer, GuestProcess
from pathlib import Path
import numpy as np

GAME_EXE     = Path(r"S:\bld\vboxgpu\tests\UltraKill\ULTRAKILL.exe")
TITLE_SUBSTR = "ULTRAKILL"
OUT_DIR      = Path(r"S:\bld\vboxgpu\automation_out\uk\probe")

def log(msg):
    print(f"[PROBE] {msg}", flush=True)

def snap(tag: str, host=None, timeout: float = 4.0):
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    dst = OUT_DIR / f"snap_{tag}.png"
    if host is not None and not host.is_alive():
        log(f"snap [{tag}] SKIP — host dead")
        return 0.0
    try:
        img = capture_host_frame(timeout=timeout)
        img.save(str(dst))
        mean = np.array(img).mean()
        log(f"snap [{tag}] mean={mean:.1f}")
        return mean
    except Exception as e:
        log(f"snap [{tag}] FAILED: {e}")
        return 0.0

def run():
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    with HostServer() as host:
        log("host started")
        with GuestProcess(GAME_EXE, extra_args=[
            "-screen-width", "800", "-screen-height", "600", "-screen-fullscreen", "0"
        ]) as guest:
            log("game started, waiting for window...")
            try:
                hwnd = find_hwnd(TITLE_SUBSTR, timeout=40.0)
            except TimeoutError as e:
                log(f"ERROR: {e}"); return
            log(f"HWND={hwnd:#x}")

            log("waiting 35s for initial load...")
            time.sleep(35.0)

            if not host.is_alive():
                log("ERROR: host died"); return

            # ESC × 2 跳过 logo
            press_escape(hwnd); time.sleep(3.0)
            press_escape(hwnd); time.sleep(3.0)

            # 到难度屏：CONTINUE at (0.50, 0.50)
            log("clicking CONTINUE...")
            click_at(hwnd, 0.50, 0.50)
            time.sleep(4.0)
            snap("00_difficulty_screen", host)

            if not host.is_alive():
                log("ERROR: host died"); return

            # ---- Y 轴扫描 ----
            # 从屏幕顶部 y=0.10 扫到 y=0.75，横向 x=0.163（STANDARD 文字中心）
            PROBE_X = 0.163
            Y_START  = 0.10
            Y_END    = 0.75
            Y_STEP   = 0.018   # ~11px 步长
            DWELL    = 0.5     # 每个位置停留 0.5s

            log(f"=== Y-scan: x={PROBE_X}, y={Y_START:.3f}..{Y_END:.3f} step={Y_STEP} ===")

            prev_y = 0.50  # 从屏幕中心开始
            y = Y_START
            while y <= Y_END + 0.001:
                # 平滑滑到目标位置（触发 WM_MOUSEMOVE hover 检测）
                glide_to(hwnd, PROBE_X, y, from_cx=PROBE_X, from_cy=prev_y, steps=8)
                time.sleep(DWELL)
                tag = f"y{y:.3f}".replace(".", "_")
                mean = snap(tag, host)
                if mean == 0.0 and host is not None and not host.is_alive():
                    log("host died during scan, stopping")
                    break
                prev_y = y
                y = round(y + Y_STEP, 4)

            log("=== Scan done. Analyze snap means to find UI rows. ===")
            log("Bright rows = UI element hover highlight.")

            # 也扫一下 x 轴（y 固定在推测的 STANDARD 位置）
            log("=== X-scan at y=0.492: find clickable width ===")
            Y_STD = 0.492
            X_START = 0.01
            X_END   = 0.40
            X_STEP  = 0.025
            prev_x = PROBE_X
            x = X_START
            while x <= X_END + 0.001:
                glide_to(hwnd, x, Y_STD, from_cx=prev_x, from_cy=Y_STD, steps=5)
                time.sleep(0.35)
                tag = f"x{x:.3f}".replace(".", "_")
                snap(tag, host)
                if not host.is_alive():
                    log("host died"); break
                prev_x = x
                x = round(x + X_STEP, 4)

            log("=== X-scan done ===")
            snap("ZZ_final", host)
            time.sleep(2.0)

if __name__ == "__main__":
    run()
