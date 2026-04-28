"""
task_uk_renderdoc_capture.py — 通过 RenderDoc 截帧 UltraKill 3D 场景

流程：
  1. 用 renderdoccmd capture 启动 host（注入 RenderDoc，在 Vulkan 设备创建前 hook）
  2. 正常启动游戏 + blind navigation 到 DOUBLE DOWN 3D 场景
  3. Focus host 窗口 + SendInput F12 触发截帧
  4. 等待 .rdc 文件出现
  5. 用 renderdoccmd thumb 导出缩略图验证内容

RenderDoc 用 WH_KEYBOARD_LL 全局 hook 拦截 F12，
只要目标进程是前台窗口就能触发。SendInput 会被全局 hook 捕获。
"""

import time, sys, os, subprocess
sys.path.insert(0, os.path.dirname(__file__))

from capture import capture_host_frame
from input import find_hwnd, click_at, press_escape, glide_to
from process import GuestProcess
from pathlib import Path
import numpy as np

GAME_EXE       = Path(r"S:\bld\vboxgpu\tests\UltraKill\ULTRAKILL.exe")
TITLE_SUBSTR   = "ULTRAKILL"
HOST_EXE       = Path(r"S:\bld\vboxgpu\build\host\Debug\vbox_host_server.exe")
HOST_CWD       = Path(r"S:\bld\vboxgpu")
HOST_TITLE     = "VBox GPU Bridge"       # partial match
RDOC_CMD       = Path(r"S:\Apps\RenderDoc\renderdoccmd.exe")
RDOC_CAPTURE   = Path(r"S:\bld\vboxgpu\rdoc_capture")   # renderdoc appends _0.rdc
LOG_DIR        = Path(r"S:\bld\vboxgpu")
OUT_DIR        = Path(r"S:\bld\vboxgpu\automation_out\uk\renderdoc")

C_CONTINUE    = (0.50,  0.50)
C_STANDARD    = (0.163, 0.496)
C_PRELUDE     = (0.50,  0.44)
C_DOUBLE_DOWN = (0.50,  0.50)

RDOC_PID_FILE   = LOG_DIR / "auto_rdoc_capture.pid"
RDOC_TRIGGER    = LOG_DIR / "rdoc_trigger.flag"

def log(msg): print(f"[RDC] {msg}", flush=True)

def _kill_pid_tree(pid: int):
    """Kill a process and its entire child tree by PID (taskkill /F /PID /T)."""
    subprocess.run(
        ["taskkill", "/F", "/PID", str(pid), "/T"],
        capture_output=True,   # silent on "not found"
    )

def _kill_stale_rdoc():
    """If a previous renderdoccmd PID file exists, kill that process tree."""
    if not RDOC_PID_FILE.exists():
        return
    try:
        pid = int(RDOC_PID_FILE.read_text().strip())
        _kill_pid_tree(pid)
        log(f"killed stale rdoc process tree (PID={pid})")
    except Exception:
        pass
    RDOC_PID_FILE.unlink(missing_ok=True)
    time.sleep(0.5)

def snap(label: str, host_proc=None, timeout: float = 5.0):
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    dst = OUT_DIR / f"{label}.png"
    if host_proc and host_proc.poll() is not None:
        log(f"snap {label} SKIP — host dead"); return 0.0
    try:
        img = capture_host_frame(timeout=timeout)
        img.save(str(dst))
        mean = float(np.array(img).mean())
        log(f"snap {label:30s}  mean={mean:.1f}")
        return mean
    except Exception as e:
        log(f"snap {label} FAILED: {e}"); return 0.0

def trigger_rdoc_capture():
    """Write trigger file — host's main loop polls for it and calls TriggerCapture()."""
    RDOC_TRIGGER.unlink(missing_ok=True)   # clear stale
    RDOC_TRIGGER.write_text("trigger")
    log(f"RenderDoc trigger file written → {RDOC_TRIGGER.name}")

def wait_for_rdc(template: Path, timeout: float = 20.0) -> Path | None:
    """Poll for <template>_frame<N>.rdc — RenderDoc API appends _frame<frameNumber>."""
    deadline = time.monotonic() + timeout
    pattern = f"{template.name}_frame*.rdc"
    while time.monotonic() < deadline:
        matches = sorted(
            (m for m in template.parent.glob(pattern) if m.stat().st_size > 1000),
            key=lambda m: m.stat().st_mtime, reverse=True,
        )
        if matches:
            return matches[0]
        time.sleep(0.5)
    return None

def wait_for_host_ready(err_file: Path, host_proc, timeout: float = 10.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if host_proc.poll() is not None:
            return False
        try:
            if "Listening on port" in err_file.read_text(encoding="utf-8", errors="ignore"):
                return True
        except Exception:
            pass
        time.sleep(0.2)
    return host_proc.poll() is None  # fallback: alive = probably ok

def run():
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    # ── 清理上次残留的 renderdoccmd+host 进程树 ───────────────────────────
    _kill_stale_rdoc()

    # ── 删除旧的 .rdc 文件，避免误认 ─────────────────────────────────────
    for old in RDOC_CAPTURE.parent.glob(f"{RDOC_CAPTURE.name}*.rdc"):
        old.unlink(missing_ok=True)
        log(f"removed old capture: {old.name}")

    # ── 通过 renderdoccmd capture 启动 host ───────────────────────────────
    # 这样 RenderDoc 在 Vulkan 设备创建前就已 hook，能正确捕帧。
    host_err_f = open(LOG_DIR / "auto_host_err.txt", "w")
    host_out_f = open(LOG_DIR / "auto_host_out.txt", "w")
    host_proc = subprocess.Popen(
        [
            str(RDOC_CMD), "capture",
            "--wait-for-exit",              # stay alive while host runs → host stderr passes through
            "--capture-file", str(RDOC_CAPTURE),
            "--opt-ref-all-resources",
            str(HOST_EXE),
        ],
        cwd=str(HOST_CWD),
        stdout=host_out_f,
        stderr=host_err_f,
    )
    log(f"host launched via renderdoccmd (PID={host_proc.pid})")
    RDOC_PID_FILE.write_text(str(host_proc.pid))   # 记录供下次清理

    if not wait_for_host_ready(LOG_DIR / "auto_host_err.txt", host_proc, timeout=15.0):
        log("ERROR: host failed to start"); host_proc.kill(); return

    log("host ready")
    rdc_path = None  # assigned inside GuestProcess block, used after

    # ── 启动游戏 + 导航 ───────────────────────────────────────────────────
    with GuestProcess(GAME_EXE, extra_args=[
        "-screen-width", "800", "-screen-height", "600", "-screen-fullscreen", "0",
    ]) as guest:
        log("game started, waiting for window...")
        try:
            game_hwnd = find_hwnd("ULTRAKILL", timeout=40.0)
        except TimeoutError as e:
            log(f"ERROR: {e}"); host_proc.kill(); return
        log(f"game HWND={game_hwnd:#x}")

        log("waiting 35s initial load...")
        time.sleep(35.0)
        if host_proc.poll() is not None: log("ERROR: host died during load"); return

        press_escape(game_hwnd); time.sleep(1.0)
        press_escape(game_hwnd); time.sleep(1.0)
        snap("00_menu", host_proc)

        def nav(coord, from_coord, wait):
            glide_to(game_hwnd, *coord, from_cx=from_coord[0], from_cy=from_coord[1], steps=8, step_delay=0.02)
            click_at(game_hwnd, *coord)
            time.sleep(wait)

        nav(C_CONTINUE,    (0.5,  0.5),           4.0); snap("01_difficulty", host_proc)
        nav(C_STANDARD,    C_CONTINUE,             3.0); snap("02_chapter",    host_proc)
        nav(C_PRELUDE,     C_STANDARD,             3.0); snap("03_level",      host_proc)
        nav(C_DOUBLE_DOWN, C_PRELUDE,             20.0); snap("04_ingame",     host_proc)

        mean = float(np.array(__import__('PIL.Image', fromlist=['Image']).open(
            str(OUT_DIR / "04_ingame.png"))).mean()) if (OUT_DIR / "04_ingame.png").exists() else 0.0
        if mean < 15.0:
            log(f"WARNING: mean={mean:.1f} — might not be in 3D scene, continuing anyway")
        else:
            log(f"In 3D scene confirmed (mean={mean:.1f})")

        # ── 截帧（写 trigger 文件 → host main loop 调 TriggerCapture()）────────
        trigger_rdoc_capture()
        log("trigger sent — waiting for .rdc file...")

        rdc_path = wait_for_rdc(RDOC_CAPTURE, timeout=20.0)
        if rdc_path:
            log(f"CAPTURE SUCCESS: {rdc_path.name} ({rdc_path.stat().st_size // 1024} KB)")
        else:
            log("CAPTURE FAILED: no .rdc file appeared within 20s")

        time.sleep(3.0)  # let game run a bit longer before shutdown

    # ── 关闭 host（杀整个进程树：renderdoccmd + host 子进程）────────────
    log("terminating host process tree...")
    _kill_pid_tree(host_proc.pid)
    try: host_proc.wait(timeout=5.0)
    except subprocess.TimeoutExpired: pass
    RDOC_PID_FILE.unlink(missing_ok=True)
    host_err_f.close(); host_out_f.close()

    # ── 导出缩略图（host 已退出，.rdc 文件已释放）────────────────────────
    if rdc_path and rdc_path.exists():
        OUT_DIR.mkdir(parents=True, exist_ok=True)
        thumb_out = OUT_DIR / "rdoc_thumb.jpg"
        result = subprocess.run(
            [str(RDOC_CMD), "thumb", "-o", str(thumb_out), str(rdc_path)],
            capture_output=True, text=True, timeout=30,
        )
        if thumb_out.exists():
            log(f"thumbnail exported: {thumb_out.name} ({thumb_out.stat().st_size} bytes)")
        else:
            log(f"thumb export failed: {result.stderr.strip()[:200]}")
    log("done")

if __name__ == "__main__":
    run()
