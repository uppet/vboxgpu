"""
task_uk_navigate_v4.py — UltraKill 全程导航（blind，固定时序）

坐标均经视觉探测确认（800×600 窗口）：
  CONTINUE       (0.50, 0.50)
  STANDARD 难度   (0.163, 0.496)  ← probe 实测，hover 触发描述面板
  PRELUDE 章节    (0.50, 0.44)    ← click_test 确认
  DOUBLE DOWN 关卡 (0.50, 0.50)   ← level 屏第3关，水平居中

流程：
  ESC×2 → CONTINUE → STANDARD → PRELUDE → DOUBLE DOWN → 3D 场景
"""

import time, sys, os
sys.path.insert(0, os.path.dirname(__file__))

from capture import capture_host_frame
from input import find_hwnd, click_at, press_escape, glide_to
from process import HostServer, GuestProcess
from pathlib import Path
import numpy as np

GAME_EXE     = Path(r"S:\bld\vboxgpu\tests\UltraKill\ULTRAKILL.exe")
TITLE_SUBSTR = "ULTRAKILL"
OUT_DIR      = Path(r"S:\bld\vboxgpu\automation_out\uk\navigate_v4")

# 全部经过实测确认的坐标
COORD_CONTINUE    = (0.50,  0.50)
COORD_STANDARD    = (0.163, 0.496)   # probe y-scan 确认
COORD_PRELUDE     = (0.50,  0.44)    # chapter screen 确认
COORD_DOUBLE_DOWN = (0.50,  0.50)    # level screen 居中，y=0.46~0.53 均在范围内

def log(msg):
    print(f"[NAV] {msg}", flush=True)

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

def nav_click(hwnd, host, label, coord, from_coord=None, glide_steps=25,
              wait_before=0.4, wait_after=3.0, snap_tag=None):
    """glide → hover wait → click → wait → optional snap."""
    if not host.is_alive():
        log(f"SKIP {label}: host dead"); return False
    cx, cy = coord
    fx, fy = from_coord if from_coord else coord
    log(f"navigating to {label} ({cx:.3f}, {cy:.3f})...")
    glide_to(hwnd, cx, cy, from_cx=fx, from_cy=fy, steps=glide_steps)
    time.sleep(wait_before)
    click_at(hwnd, cx, cy)
    time.sleep(wait_after)
    if snap_tag:
        snap(snap_tag, host)
    return host.is_alive()

def run():
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    host = HostServer()
    host.ensure_running()
    log("host ready")

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
        snap("00_after_load", host)
        if not host.is_alive(): log("ERROR: host died during load"); return

        log("ESC × 2 to skip logos...")
        press_escape(hwnd); time.sleep(3.0)
        press_escape(hwnd); time.sleep(3.0)
        snap("01_main_menu", host)
        if not host.is_alive(): return

        if not nav_click(hwnd, host, "CONTINUE", COORD_CONTINUE,
                         from_coord=(0.50, 0.50), wait_after=4.0,
                         snap_tag="02_difficulty"):
            return

        if not nav_click(hwnd, host, "STANDARD", COORD_STANDARD,
                         from_coord=COORD_CONTINUE, glide_steps=30,
                         wait_before=0.5, wait_after=3.0,
                         snap_tag="03_chapter"):
            return

        if not nav_click(hwnd, host, "PRELUDE", COORD_PRELUDE,
                         from_coord=(0.50, 0.50), glide_steps=20,
                         wait_before=0.4, wait_after=3.0,
                         snap_tag="04_level"):
            return

        if not nav_click(hwnd, host, "DOUBLE DOWN", COORD_DOUBLE_DOWN,
                         from_coord=COORD_PRELUDE, glide_steps=15,
                         wait_before=0.4, wait_after=5.0,
                         snap_tag="05_loading"):
            return

        log("waiting 20s for 3D scene to load...")
        time.sleep(20.0)
        snap("06_ingame", host)

        if host.is_alive():
            log("SUCCESS: in-game, keeping alive 10s...")
            time.sleep(10.0)
            snap("07_ingame_final", host)
        else:
            log("ERROR: host died before in-game")

if __name__ == "__main__":
    run()
