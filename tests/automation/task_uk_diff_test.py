"""
task_uk_diff_test.py — 专门测试难度选择屏的交互方式

目标：找到能从 DIFFICULTY 屏导航走的操作。

测试顺序：
  1. 启动 host + game，到达难度选择屏
  2. 尝试不同交互方法，每次等 6 秒 + 快照
  3. 如果屏幕变化，记录是哪个操作有效

交互方法顺序：
  A. glide 到 STANDARD + click（鼠标滑行后点击）
  B. 8 秒静待（auto-advance？）
  C. ENTER 键（确认当前选择？）
  D. glide 到 HARMLESS + click（尝试另一个 diff）
  E. 再 glide 回 STANDARD + click
  F. glide 到 0-3 数字区域（x≈0.19）+ click
  G. glide 到 checkbox 区（x≈0.295）+ click
  H. WM_LBUTTONDBLCLK（双击）
"""

import time, sys, os, win32con
sys.path.insert(0, os.path.dirname(__file__))

from capture import capture_host_frame
from input import find_hwnd, click_at, press_escape, press_key, focus_window, glide_to
from process import HostServer, GuestProcess
from pathlib import Path
import numpy as np
import ctypes
import win32gui

GAME_EXE     = Path(r"S:\bld\vboxgpu\tests\UltraKill\ULTRAKILL.exe")
TITLE_SUBSTR = "ULTRAKILL"
OUT_DIR      = Path(r"S:\bld\vboxgpu\automation_out\uk\diff_test")

def log(msg):
    print(f"[DIFF] {msg}", flush=True)

def snap(tag: str, host=None, timeout: float = 5.0):
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    dst = OUT_DIR / f"snap_{tag}.png"
    if host is not None and not host.is_alive():
        log(f"snap [{tag}] SKIP — host dead")
        return None, 0.0
    try:
        img = capture_host_frame(timeout=timeout)
        img.save(str(dst))
        mean = np.array(img).mean()
        log(f"snap [{tag}] mean={mean:.1f}")
        return dst, mean
    except Exception as e:
        log(f"snap [{tag}] FAILED: {e}")
        return dst, 0.0

def dblclick_at(hwnd, cx, cy):
    """Send WM_LBUTTONDBLCLK to the window."""
    left, top, right, bottom = win32gui.GetClientRect(hwnd)
    w = right - left
    h = bottom - top
    lx = int(w * cx)
    ly = int(h * cy)
    lparam = (ly << 16) | (lx & 0xFFFF)
    focus_window(hwnd)
    win32gui.PostMessage(hwnd, win32con.WM_LBUTTONDBLCLK, win32con.MK_LBUTTON, lparam)

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

            # 等初始加载 + logo 自动消失
            log("waiting 35s for initial load...")
            time.sleep(35.0)
            snap("00_initial", host)

            if not host.is_alive():
                log("ERROR: host died"); return

            # ESC × 2 (safe, PostMessage goes to game)
            press_escape(hwnd); time.sleep(3.0)
            press_escape(hwnd); time.sleep(3.0)
            snap("01_pre_continue", host)

            # 到达难度屏：CONTINUE at (0.50, 0.50)
            log("=== Getting to DIFFICULTY screen ===")
            click_at(hwnd, 0.50, 0.50)
            time.sleep(4.0)
            snap("02_difficulty_screen", host)

            if not host.is_alive():
                log("ERROR: host died"); return

            # STANDARD 坐标：文字中心约 (0.163, 0.465)
            STD_X, STD_Y = 0.163, 0.465
            # HARMLESS: (0.163, 0.292)
            # 0-3 数字区域: (0.190, 0.465)
            # Checkbox: (0.295, 0.465)

            # --- Test A: glide → STANDARD + click ---
            log("=== Test A: glide to STANDARD + click ===")
            glide_to(hwnd, STD_X, STD_Y, from_cx=0.5, from_cy=0.5, steps=30)
            snap("A1_hover_std", host)
            click_at(hwnd, STD_X, STD_Y)
            time.sleep(6.0)
            snap("A2_after_click", host)

            # --- Test B: 8s static wait (auto-advance?) ---
            log("=== Test B: 8s wait (auto-advance?) ===")
            time.sleep(8.0)
            snap("B1_after_8s_wait", host)

            # --- Test C: ENTER ---
            log("=== Test C: ENTER key ===")
            press_key(hwnd, win32con.VK_RETURN)
            time.sleep(5.0)
            snap("C1_after_enter", host)

            if not host.is_alive():
                log("ERROR: host died at test C"); return

            # 如果 ENTER 送回主菜单，重新 CONTINUE
            # 检查 mean，若很低（< 3）重新导航
            _, mean_c = snap("C_check", host)
            if mean_c < 3.0:
                log("Looks black/menu - re-navigating to difficulty...")
                click_at(hwnd, 0.50, 0.50)
                time.sleep(4.0)

            # --- Test D: glide to HARMLESS + click ---
            log("=== Test D: glide to HARMLESS + click ===")
            glide_to(hwnd, 0.163, 0.292, from_cx=STD_X, from_cy=STD_Y, steps=30)
            click_at(hwnd, 0.163, 0.292)
            time.sleep(5.0)
            snap("D1_after_harmless", host)

            if not host.is_alive():
                log("ERROR: host died at D"); return

            # --- Test E: glide back to STANDARD + click ---
            log("=== Test E: glide back to STANDARD + click ===")
            glide_to(hwnd, STD_X, STD_Y, from_cx=0.163, from_cy=0.292, steps=30)
            click_at(hwnd, STD_X, STD_Y)
            time.sleep(5.0)
            snap("E1_after_std_again", host)

            # --- Test F: click on "0-3" number area ---
            log("=== Test F: click on '0-3' number area (x=0.19) ===")
            glide_to(hwnd, 0.190, STD_Y, from_cx=STD_X, from_cy=STD_Y, steps=15)
            click_at(hwnd, 0.190, STD_Y)
            time.sleep(5.0)
            snap("F1_after_number_click", host)

            # --- Test G: click on checkbox [□] ---
            log("=== Test G: click on checkbox area (x=0.295) ===")
            glide_to(hwnd, 0.295, STD_Y, from_cx=0.190, from_cy=STD_Y, steps=15)
            click_at(hwnd, 0.295, STD_Y)
            time.sleep(5.0)
            snap("G1_after_checkbox", host)

            # --- Test H: double-click STANDARD ---
            log("=== Test H: double-click STANDARD ===")
            glide_to(hwnd, STD_X, STD_Y, from_cx=0.295, from_cy=STD_Y, steps=15)
            dblclick_at(hwnd, STD_X, STD_Y)
            time.sleep(5.0)
            snap("H1_after_dblclick", host)

            log("=== All tests done ===")
            snap("ZZ_final_state", host)
            time.sleep(3.0)

if __name__ == "__main__":
    run()
