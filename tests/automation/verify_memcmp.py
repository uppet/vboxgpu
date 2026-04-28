"""Quick verify: start host+UK at 1280x720, wait, capture game window, check for glitch."""
import time, sys, os
sys.path.insert(0, os.path.dirname(__file__))

from capture import capture_window
from input import find_hwnd
from process import HostServer, GuestProcess
from pathlib import Path
import numpy as np

GAME_EXE = Path(r"S:\bld\vboxgpu\tests\UltraKill\ULTRAKILL.exe")
OUT_DIR  = Path(r"S:\bld\vboxgpu\automation_out\verify_gather_diag")

def log(msg): print(f"[VERIFY] {msg}", flush=True)

def snap(label):
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    dst = OUT_DIR / f"{label}.png"
    try:
        # Capture ULTRAKILL game window (host Client window has WS_EX_TOOLWINDOW, WGC fails)
        img = capture_window("ULTRAKILL")
        img.save(str(dst))
        arr = np.array(img)
        mean = float(arr.mean())
        log(f"snap {label:30s}  size={img.size}  mean={mean:.1f}")
        return img, mean
    except Exception as e:
        log(f"snap {label} FAILED: {e}")
        return None, 0.0

def run():
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    with HostServer() as host:
        log("host started")
        with GuestProcess(GAME_EXE, extra_args=[
            "-screen-width", "1280", "-screen-height", "720",
            "-screen-fullscreen", "0",
        ]) as guest:
            log("waiting for game window...")
            try:
                hwnd = find_hwnd("ULTRAKILL", timeout=40.0)
            except TimeoutError as e:
                log(f"ERROR: {e}"); return
            log(f"game HWND={hwnd:#x}")

            # Wait for shader compile + logo
            log("waiting 35s for initial load + rendering...")
            time.sleep(35)
            if not host.is_alive():
                log("ERROR: host died during load"); return

            # Take multiple snapshots over time to detect intermittent glitch
            for i in range(3):
                snap(f"frame_{i:02d}")
                time.sleep(1)

            log("done — check screenshots in automation_out/verify_memcmp/")

if __name__ == "__main__":
    run()
