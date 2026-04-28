"""
task_verify_perf_navigate.py — 验证线程池优化：启动 UltraKill + ESC 导航到菜单 + WGC 截图

流程：等 logo → ESC×2 → 截图菜单 → 报告
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(__file__))

from process import HostServer, GuestProcess
from capture import capture_window, capture_host_frame
from input import find_hwnd, press_escape
from pathlib import Path
import numpy as np

GAME_EXE = Path(r"S:\bld\vboxgpu\tests\UltraKill\ULTRAKILL.exe")
GAME_ARGS = ["-screen-width", "800", "-screen-height", "600", "-screen-fullscreen", "0"]
OUT_DIR = Path(r"S:\bld\vboxgpu\automation_out\verify_navigate")
HOST_TITLE = "VBox GPU Bridge"
LOG_DIR = Path(r"S:\bld\vboxgpu")

def log(msg):
    print(f"[NAV] {time.strftime('%H:%M:%S')} {msg}", flush=True)

def snap(label):
    """WGC capture_helper 截游戏窗口（ULTRAKILL 窗口有 guest 回传画面）。"""
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    dst = OUT_DIR / f"{label}.png"
    try:
        img = capture_window("ULTRAKILL", out_path=dst)
        mean = float(np.array(img).mean())
        log(f"  snap {label}: mean={mean:.1f} size={img.size}")
        return img, mean
    except Exception as e:
        log(f"  snap {label}: FAILED ({e})")
        return None, 0.0

def get_fps():
    try:
        lines = (LOG_DIR / "auto_host_err.txt").read_text(encoding='utf-8', errors='replace').splitlines()
        for line in reversed(lines):
            if "FPS:" in line:
                return line.split("FPS:")[1].strip()
    except: pass
    return None

def run():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    log("=== Navigate test ===")

    with HostServer() as host:
        log(f"host alive={host.is_alive()}")
        with GuestProcess(GAME_EXE, extra_args=GAME_ARGS) as guest:
            log("guest started")

            # Wait for game window
            try:
                hwnd = find_hwnd("ULTRAKILL", timeout=30.0)
                log(f"game window HWND={hwnd:#x}")
            except TimeoutError:
                log("ERROR: game window not found"); return

            # Wait for loading (logo video + shader compile)
            log("waiting 15s for initial load...")
            for t in range(15):
                time.sleep(1)
                if not host.is_alive():
                    log(f"HOST DIED at t={t+1}s!"); snap("host_dead"); return
                if not guest.is_alive():
                    log(f"GUEST DIED at t={t+1}s!"); return

            snap("01_after_load")
            fps = get_fps()
            log(f"FPS after load: {fps}")

            # ESC twice to skip logo/violence notice
            log("ESC 1")
            press_escape(hwnd)
            time.sleep(1.5)
            if not host.is_alive(): log("HOST DIED after ESC1"); snap("esc1_dead"); return
            snap("02_after_esc1")

            log("ESC 2")
            press_escape(hwnd)
            time.sleep(2.0)
            if not host.is_alive(): log("HOST DIED after ESC2"); snap("esc2_dead"); return
            _, mean = snap("03_after_esc2_menu")
            fps = get_fps()
            log(f"Menu screen: mean={mean:.1f} FPS={fps}")

            # Observe for a few seconds
            time.sleep(3)
            _, mean = snap("04_menu_stable")
            fps = get_fps()
            log(f"Menu stable: mean={mean:.1f} FPS={fps}")

            if mean > 30:
                log("SUCCESS — menu visible, rendering working")
            else:
                log(f"UNCERTAIN — mean={mean:.1f}, might be black")

            log("Done.")

if __name__ == "__main__":
    run()
