"""
task_uk_chapter_probe.py — 探测 CHAPTER 屏和 LEVEL 屏坐标

流程：
  1. 到 CHAPTER 屏（STANDARD 已确认可点击）
  2. 在 x=0.50 处 Y-scan：hover 出现描述面板 → 确认 PRELUDE 精确坐标
  3. 点击 PRELUDE，记录 LEVEL 屏截图
  4. 在 LEVEL 屏做 Y-scan + X-scan 找 DOUBLE DOWN
"""

import time, sys, os, win32con
sys.path.insert(0, os.path.dirname(__file__))

from capture import capture_host_frame
from input import find_hwnd, click_at, press_escape, focus_window, glide_to
from process import HostServer, GuestProcess
from pathlib import Path
import numpy as np

GAME_EXE     = Path(r"S:\bld\vboxgpu\tests\UltraKill\ULTRAKILL.exe")
TITLE_SUBSTR = "ULTRAKILL"
OUT_DIR      = Path(r"S:\bld\vboxgpu\automation_out\uk\chapter_probe")

STD_X, STD_Y = 0.163, 0.496  # 已确认

def log(msg):
    print(f"[CP] {msg}", flush=True)

def snap(tag: str, host=None, timeout: float = 5.0):
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    dst = OUT_DIR / f"snap_{tag}.png"
    if host is not None and not host.is_alive():
        log(f"snap [{tag}] SKIP — host dead"); return 0.0
    try:
        img = capture_host_frame(timeout=timeout)
        img.save(str(dst))
        mean = np.array(img).mean()
        log(f"snap [{tag}] mean={mean:.1f}")
        return mean
    except Exception as e:
        log(f"snap [{tag}] FAILED: {e}"); return 0.0

def y_scan(hwnd, host, x, y_start, y_end, y_step, prefix, dwell=0.45):
    """在固定 x 处从 y_start 扫到 y_end，每步截图。"""
    prev_y = y_start
    y = y_start
    while y <= y_end + 0.001:
        glide_to(hwnd, x, y, from_cx=x, from_cy=prev_y, steps=6)
        time.sleep(dwell)
        tag = f"{prefix}_y{y:.3f}".replace(".", "_")
        snap(tag, host)
        if not host.is_alive(): break
        prev_y = y
        y = round(y + y_step, 4)

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

            log("waiting 35s...")
            time.sleep(35.0)
            if not host.is_alive(): log("host died"); return

            press_escape(hwnd); time.sleep(3.0)
            press_escape(hwnd); time.sleep(3.0)

            # → DIFFICULTY
            log("CONTINUE → DIFFICULTY...")
            click_at(hwnd, 0.50, 0.50)
            time.sleep(4.0)
            snap("00_difficulty", host)

            # → CHAPTER（点 STANDARD，已确认坐标）
            log("glide + click STANDARD...")
            glide_to(hwnd, STD_X, STD_Y, from_cx=0.50, from_cy=0.50, steps=25)
            time.sleep(0.4)
            click_at(hwnd, STD_X, STD_Y)
            time.sleep(2.5)
            snap("01_chapter_screen", host)
            if not host.is_alive(): log("host died"); return

            # ---- CHAPTER 屏 Y-scan at x=0.50 ----
            log("=== CHAPTER Y-scan at x=0.50 ===")
            y_scan(hwnd, host, x=0.50, y_start=0.20, y_end=0.70,
                   y_step=0.025, prefix="ch")

            snap("02_chapter_after_scan", host)
            if not host.is_alive(): log("host died"); return

            # 视觉确认 PRELUDE ≈ (0.50, 0.44)，点击它
            log("clicking PRELUDE (0.50, 0.44)...")
            glide_to(hwnd, 0.50, 0.44, from_cx=0.50, from_cy=0.50, steps=15)
            time.sleep(0.4)
            snap("03_hover_prelude", host)
            click_at(hwnd, 0.50, 0.44)
            time.sleep(2.5)
            snap("04_level_screen", host)
            if not host.is_alive(): log("host died"); return

            # ---- LEVEL 屏 Y-scan at x=0.50 ----
            log("=== LEVEL Y-scan at x=0.50 ===")
            y_scan(hwnd, host, x=0.50, y_start=0.20, y_end=0.70,
                   y_step=0.025, prefix="lv")

            snap("05_level_after_scan", host)
            if not host.is_alive(): log("host died"); return

            # 也做一下 X-scan 在 y=0.50（DOUBLE DOWN 可能在中间）
            log("=== LEVEL X-scan at y=0.50 ===")
            prev_x = 0.50
            x = 0.10
            while x <= 0.90 + 0.001:
                glide_to(hwnd, x, 0.50, from_cx=prev_x, from_cy=0.50, steps=5)
                time.sleep(0.35)
                tag = f"lv_x{x:.3f}".replace(".", "_")
                snap(tag, host)
                if not host.is_alive(): break
                prev_x = x
                x = round(x + 0.08, 4)

            snap("ZZ_final", host)
            log("done")
            time.sleep(2.0)

if __name__ == "__main__":
    run()
