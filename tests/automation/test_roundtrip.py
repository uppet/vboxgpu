"""Roundtrip test: record UK command stream → replay locally → screenshot.
If replay has glitches = encode/decode loses info.
If replay is clean = issue is elsewhere."""
import time, sys, os, subprocess
sys.path.insert(0, os.path.dirname(__file__))

from capture import capture_window
from input import find_hwnd
from process import GuestProcess, _host_env
from pathlib import Path
import numpy as np

GAME_EXE   = Path(r"S:\bld\vboxgpu\tests\UltraKill\ULTRAKILL.exe")
HOST_EXE   = Path(r"S:\bld\vboxgpu\build\host\Debug\vbox_host_server.exe")
DUMP_FILE  = Path(r"S:\bld\vboxgpu\uk_roundtrip.bin")
OUT_DIR    = Path(r"S:\bld\vboxgpu\automation_out\roundtrip_test")
HOST_CWD   = Path(r"S:\bld\vboxgpu")

def log(msg): print(f"[RT] {msg}", flush=True)

def snap(label, title="Client"):
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    dst = OUT_DIR / f"{label}.png"
    try:
        img = capture_window(title)
        img.save(str(dst))
        log(f"snap {label} OK size={img.size} mean={float(np.array(img).mean()):.1f}")
        return True
    except Exception as e:
        log(f"snap {label} FAILED: {e}")
        return False

def step1_record():
    """Record ~10s of UK main menu."""
    log("=== STEP 1: Record ===")
    DUMP_FILE.unlink(missing_ok=True)

    # Start host with --dump via 'start' (same as man_run.bat pattern)
    host_cmd = f'start "" /D "{HOST_CWD}" "{HOST_EXE}" --dump "{DUMP_FILE}"'
    subprocess.Popen(host_cmd, shell=True, env=_host_env())
    time.sleep(2)

    with GuestProcess(GAME_EXE, extra_args=[
        "-screen-width", "1280", "-screen-height", "720",
        "-screen-fullscreen", "0",
    ]) as guest:
        try:
            hwnd = find_hwnd("ULTRAKILL", timeout=40.0)
        except TimeoutError as e:
            log(f"ERROR: {e}"); return False
        log(f"game HWND={hwnd:#x}")
        log("waiting 45s for load + recording...")
        time.sleep(45)

    # Stop host
    subprocess.run("taskkill /F /IM vbox_host_server.exe",
                    shell=True, capture_output=True)
    time.sleep(2)

    if DUMP_FILE.exists():
        sz = DUMP_FILE.stat().st_size
        log(f"dump: {sz / 1024 / 1024:.1f} MB")
        return sz > 1000
    log("ERROR: no dump"); return False

def step2_replay():
    """Replay and screenshot."""
    log("=== STEP 2: Replay ===")
    # Start host in replay mode
    replay_cmd = f'start "" /D "{HOST_CWD}" "{HOST_EXE}" --replay "{DUMP_FILE}"'
    subprocess.Popen(replay_cmd, shell=True, env=_host_env())
    log("replay started, waiting 10s for render...")
    time.sleep(10)

    for i in range(5):
        snap(f"replay_{i:02d}")
        time.sleep(2)

    subprocess.run("taskkill /F /IM vbox_host_server.exe",
                    shell=True, capture_output=True)

def run():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    if step1_record():
        step2_replay()
    log("=== Done — check automation_out/roundtrip_test/ ===")

if __name__ == "__main__":
    run()
