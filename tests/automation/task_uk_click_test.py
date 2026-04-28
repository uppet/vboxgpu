"""
task_uk_click_test.py — 用正确坐标点击 STANDARD，验证是否导航到下一屏

探测结果（800×600 窗口）：
  STANDARD 行: x=0.01-0.31, y≈0.490-0.506（中心 0.163, 0.496）
  hover 时右侧出现 "--STANDARD--" 描述面板

本脚本验证点击后是否进入章节选择屏（PRELUDE 等），
同时对后续屏幕截图以确定 PRELUDE 和 DOUBLE DOWN 坐标。
"""

import time, sys, os, win32con
sys.path.insert(0, os.path.dirname(__file__))

from capture import capture_host_frame
from input import find_hwnd, click_at, press_escape, press_key, focus_window, glide_to
from process import HostServer, GuestProcess
from pathlib import Path
import numpy as np

GAME_EXE     = Path(r"S:\bld\vboxgpu\tests\UltraKill\ULTRAKILL.exe")
TITLE_SUBSTR = "ULTRAKILL"
OUT_DIR      = Path(r"S:\bld\vboxgpu\automation_out\uk\click_test")

# 已确认的坐标（probe 实测）
STD_X, STD_Y = 0.163, 0.496

def log(msg):
    print(f"[CLK] {msg}", flush=True)

def snap(tag: str, host=None, timeout: float = 5.0):
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

            # 点击 CONTINUE
            log("clicking CONTINUE (0.50, 0.50)...")
            click_at(hwnd, 0.50, 0.50)
            time.sleep(4.0)
            snap("00_difficulty_screen", host)

            if not host.is_alive():
                log("ERROR: host died"); return

            # glide 到 STANDARD（y=0.496，已由 probe 实测确认）
            log(f"gliding to STANDARD ({STD_X}, {STD_Y})...")
            glide_to(hwnd, STD_X, STD_Y, from_cx=0.50, from_cy=0.50, steps=30)
            time.sleep(0.5)  # 等 hover 描述面板出现
            snap("01_hover_std", host)

            # 确认 hover 后点击
            log("clicking STANDARD...")
            click_at(hwnd, STD_X, STD_Y)
            time.sleep(1.0)
            snap("02_after_click1", host)
            time.sleep(3.0)
            snap("03_after_3s", host)

            if not host.is_alive():
                log("ERROR: host died after clicking STANDARD"); return

            # 如果还在难度屏：再点一次 + SPACE
            log("second click + SPACE...")
            click_at(hwnd, STD_X, STD_Y)
            time.sleep(0.3)
            press_key(hwnd, win32con.VK_SPACE)
            time.sleep(3.0)
            snap("04_after_space", host)

            if not host.is_alive():
                log("ERROR: host died"); return

            # 不管在哪个屏幕，拍一系列照片记录当前状态
            log("=== Documenting current state ===")
            for i in range(6):
                time.sleep(1.5)
                snap(f"state_{i:02d}", host)
                if not host.is_alive():
                    log("host died"); break

            # 如果到了章节选择屏，尝试探测 PRELUDE 位置
            # PRELUDE 在 ULTRAKILL 中通常是第一个章节（PRELUDE: THE MOUTH OF HELL）
            # 坐标需视截图而定，先试 (0.163, 0.35) 和中间位置
            log("=== Attempting PRELUDE navigation ===")
            # 尝试几个可能的 PRELUDE 位置
            for try_y in [0.30, 0.35, 0.40, 0.45]:
                log(f"trying glide to ({0.163}, {try_y})...")
                glide_to(hwnd, 0.163, try_y, from_cx=0.163, from_cy=STD_Y, steps=10)
                time.sleep(0.4)
                snap(f"prelude_probe_{try_y:.2f}".replace(".", "_"), host)
                if not host.is_alive():
                    log("host died"); break

            log("done")
            time.sleep(2.0)

if __name__ == "__main__":
    run()
