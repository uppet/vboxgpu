"""
task_verify_perf.py — 验证 host 性能优化的正确性脚本

精确监控 host/guest 存活，每秒轮询一次：
  - host 死亡 → 打印死亡时间 + 最后 stderr
  - guest 死亡 → 打印 exit_code
  - 两者都活 → 尝试截图 host 窗口判断渲染状态

用法: python task_verify_perf.py [ultrakill|sortcourt]
"""

import sys
import os
import time
import subprocess
from pathlib import Path

sys.path.insert(0, os.path.dirname(__file__))
from process import HostServer, GuestProcess
from capture import capture_window, is_black

GAME_EXE = {
    "ultrakill": Path(r"S:\bld\vboxgpu\tests\UltraKill\ULTRAKILL.exe"),
    "sortcourt": Path(r"S:\bld\vboxgpu\tests\SortTheCourt\SortTheCourt.exe"),
}
GAME_ARGS = ["-screen-width", "800", "-screen-height", "600", "-screen-fullscreen", "0"]

LOG_DIR = Path(r"S:\bld\vboxgpu")
OUT_DIR = Path(r"S:\bld\vboxgpu\automation_out\verify_perf")

def log(msg):
    print(f"[VERIFY] {time.strftime('%H:%M:%S')} {msg}", flush=True)

def read_host_err_tail(n=10):
    """Read last n lines of host stderr log."""
    p = LOG_DIR / "auto_host_err.txt"
    try:
        lines = p.read_text(encoding='utf-8', errors='replace').splitlines()
        return lines[-n:]
    except Exception:
        return []

def count_host_batches():
    """Count batches processed (lines containing 'T4')."""
    p = LOG_DIR / "auto_host_err.txt"
    try:
        text = p.read_text(encoding='utf-8', errors='replace')
        return text.count('] T4 @')
    except Exception:
        return 0

def get_host_fps():
    """Get latest FPS from host log."""
    p = LOG_DIR / "auto_host_err.txt"
    try:
        lines = p.read_text(encoding='utf-8', errors='replace').splitlines()
        for line in reversed(lines):
            if "FPS:" in line:
                return line.split("FPS:")[1].strip()
    except Exception:
        pass
    return None

HOST_WINDOW_TITLE = "VBox GPU Bridge"

def snap(label):
    """WGC 进程外截图 host 窗口。"""
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    dst = OUT_DIR / f"{label}.png"
    try:
        img = capture_window(HOST_WINDOW_TITLE, out_path=dst)
        import numpy as np
        mean = float(np.array(img).mean())
        black = is_black(img)
        log(f"  snap {label}: mean={mean:.1f} black={black} size={img.size} → {dst.name}")
        return mean, black
    except Exception as e:
        log(f"  snap {label}: FAILED ({e})")
        return 0.0, True

def deploy_icd(game_name):
    """Copy latest ICD DLL to game directory."""
    src = Path(r"S:\bld\vboxgpu\build\guest_vk_icd\Debug\vbox_vulkan.dll")
    if game_name == "sortcourt":
        src = Path(r"S:\bld\vboxgpu\build32\guest_vk_icd\Debug\vbox_vulkan.dll")
    dst = GAME_EXE[game_name].parent / "vbox_vulkan.dll"
    try:
        import shutil
        shutil.copy2(str(src), str(dst))
        log(f"ICD deployed: {src.name} → {dst.parent.name}/")
    except Exception as e:
        log(f"ICD deploy FAILED: {e}")

def run(game_name="ultrakill", observe_seconds=30):
    log(f"=== Verify perf: {game_name} ({observe_seconds}s) ===")

    deploy_icd(game_name)

    host = HostServer()
    host.ensure_running()
    log(f"host ready (PID alive={host.is_alive()})")
    try:

        with GuestProcess(GAME_EXE[game_name], extra_args=GAME_ARGS) as guest:
            log(f"guest started: {GAME_EXE[game_name].name}")

            t0 = time.monotonic()
            last_batch_count = 0
            first_frame_time = None

            while time.monotonic() - t0 < observe_seconds:
                time.sleep(1.0)
                elapsed = time.monotonic() - t0

                h_alive = host.is_alive()
                g_alive = guest.is_alive()
                batches = count_host_batches()
                fps = get_host_fps()

                status = f"t={elapsed:.0f}s host={'OK' if h_alive else 'DEAD'} guest={'OK' if g_alive else 'DEAD'} batches={batches}"
                if fps:
                    status += f" FPS={fps}"
                log(status)

                # Host 死了 — 打印死因并截图
                if not h_alive:
                    log("HOST DIED! Last stderr:")
                    for line in read_host_err_tail(5):
                        log(f"  | {line}")
                    snap(f"host_dead_t{int(elapsed)}")
                    break

                # Guest 死了
                if not g_alive:
                    rc = guest._proc.returncode if guest._proc else -1
                    log(f"GUEST DIED! exit_code={rc} (0x{rc & 0xFFFFFFFF:08X})")
                    for line in read_host_err_tail(5):
                        log(f"  | {line}")
                    break

                # 定期截图（t=5s, 10s, 15s, 20s）
                if int(elapsed) in (5, 10, 15, 20) and int(elapsed) != getattr(run, '_last_snap', -1):
                    run._last_snap = int(elapsed)
                    snap(f"t{int(elapsed)}")

                # 有新 batch → 尝试截图
                if batches > last_batch_count + 50 and first_frame_time is None:
                    first_frame_time = elapsed
                    log(f"  first rendering detected at t={elapsed:.1f}s")
                    snap("first_frame")

                last_batch_count = batches

            # 结束 — 最终状态
            if host.is_alive() and guest.is_alive():
                log("=== OBSERVATION COMPLETE (both alive) ===")
                fps = get_host_fps()
                batches = count_host_batches()
                log(f"Final: batches={batches} FPS={fps}")
                snap("final")

            log("Stopping guest...")
    finally:
        pass  # host 常驻，不 stop
    log("Done.")

if __name__ == "__main__":
    game = sys.argv[1] if len(sys.argv) > 1 else "ultrakill"
    observe = int(sys.argv[2]) if len(sys.argv) > 2 else 30
    run(game, observe)
