"""
task_uk_blind_run.py — UltraKill 盲打快速导航脚本

游戏就绪后 3 秒开始，按固定时序依次执行 UI 操作，每步截图记录。
不做视觉判断，纯靠时序。

时序说明：
  - ESC、ESC：1 秒间隔（按键响应快）
  - CONTINUE：按后等 4 秒（难度屏加载）
  - STANDARD：按后等 3 秒（章节屏加载）
  - PRELUDE：按后等 3 秒（关卡屏加载）
  - DOUBLE DOWN：按后等 20 秒（3D 场景加载）

注意：首次冷启动（shader 未缓存）时 host 可能在加载期崩溃，
      重跑一次即可（第二次缓存已建立，稳定通过）。

坐标全部经 glide probe 实测（800×600 窗口）。
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
OUT_DIR      = Path(r"S:\bld\vboxgpu\automation_out\uk\blind_run")

# ── 实测坐标（800×600）──────────────────────────────────────────────────────
C_CONTINUE    = (0.50,  0.50)
C_STANDARD    = (0.163, 0.496)
C_PRELUDE     = (0.50,  0.44)
C_DOUBLE_DOWN = (0.50,  0.50)

LOAD_WAIT  = 35.0   # shader 编译等待（有缓存时实际 ~10s，保守取 35s）
INIT_WAIT  =  3.0   # 就绪后再等 3 秒缓冲

def log(msg): print(f"[BLIND] {msg}", flush=True)

def snap(label: str, host=None):
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    dst = OUT_DIR / f"{label}.png"
    if host and not host.is_alive():
        log(f"snap {label} SKIP — host dead"); return 0.0
    try:
        img = capture_host_frame(timeout=5.0)
        img.save(str(dst))
        mean = float(np.array(img).mean())
        log(f"snap {label:30s}  mean={mean:.1f}")
        return mean
    except Exception as e:
        log(f"snap {label} FAILED: {e}"); return 0.0

def act(label: str, fn, wait: float, host, snap_label: str):
    """执行动作 → 等待 → 截图。"""
    log(f"─── {label}")
    fn()
    time.sleep(wait)
    snap(snap_label, host)
    return host.is_alive() if host else True

def run():
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    with HostServer() as host:
        log("host started")
        with GuestProcess(GAME_EXE, extra_args=[
            "-screen-width", "800", "-screen-height", "600",
            "-screen-fullscreen", "0",
        ]) as guest:

            # 等游戏窗口
            log("waiting for game window...")
            try:
                hwnd = find_hwnd(TITLE_SUBSTR, timeout=40.0)
            except TimeoutError as e:
                log(f"ERROR: {e}"); return
            log(f"window HWND={hwnd:#x}")

            # 等 shader 编译 + logo 自动消失
            log(f"waiting {LOAD_WAIT:.0f}s for initial load...")
            time.sleep(LOAD_WAIT)
            if not host.is_alive(): log("ERROR: host died during load"); return

            # 就绪缓冲
            log(f"ready — waiting {INIT_WAIT:.0f}s before first action...")
            time.sleep(INIT_WAIT)
            snap("00_ready", host)

            # ── 快速序列 ────────────────────────────────────────────────────

            if not act("ESC 1 (skip logo screen)",
                       lambda: press_escape(hwnd), 1.0, host, "01_esc1"): return

            if not act("ESC 2 (skip violence notice)",
                       lambda: press_escape(hwnd), 1.0, host, "02_esc2"): return

            if not act("CONTINUE",
                       lambda: click_at(hwnd, *C_CONTINUE), 4.0, host, "03_difficulty"): return

            if not act("STANDARD (glide+click)",
                       lambda: (
                           glide_to(hwnd, *C_STANDARD,
                                    from_cx=C_CONTINUE[0], from_cy=C_CONTINUE[1],
                                    steps=8, step_delay=0.02),
                           click_at(hwnd, *C_STANDARD),
                       ), 3.0, host, "04_chapter"): return

            if not act("PRELUDE (glide+click)",
                       lambda: (
                           glide_to(hwnd, *C_PRELUDE,
                                    from_cx=C_STANDARD[0], from_cy=C_STANDARD[1],
                                    steps=8, step_delay=0.02),
                           click_at(hwnd, *C_PRELUDE),
                       ), 3.0, host, "05_level"): return

            if not act("DOUBLE DOWN (glide+click)",
                       lambda: (
                           glide_to(hwnd, *C_DOUBLE_DOWN,
                                    from_cx=C_PRELUDE[0], from_cy=C_PRELUDE[1],
                                    steps=8, step_delay=0.02),
                           click_at(hwnd, *C_DOUBLE_DOWN),
                       ), 20.0, host, "06_loading_or_ingame"): return

            # ── 确认进入 3D 场景 ────────────────────────────────────────────
            mean = snap("07_ingame_check", host)
            if mean > 15.0:
                log(f"SUCCESS — in-game (mean={mean:.1f})")
            else:
                log(f"UNCERTAIN — mean={mean:.1f}, check screenshots")

            log("keeping alive 10s for observation...")
            time.sleep(10.0)
            snap("08_ingame_final", host)

if __name__ == "__main__":
    run()
