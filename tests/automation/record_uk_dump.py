"""Record a short UltraKill command stream dump for RenderDoc replay analysis."""
import time, sys, os
sys.path.insert(0, os.path.dirname(__file__))

from process import HostServer, GuestProcess
from input import find_hwnd
from pathlib import Path

GAME_EXE  = Path(r"S:\bld\vboxgpu\tests\UltraKill\ULTRAKILL.exe")
DUMP_FILE = Path(r"S:\bld\vboxgpu\uk_renderdoc.bin")

def log(msg): print(f"[REC] {msg}", flush=True)

def run():
    # Delete old dump
    DUMP_FILE.unlink(missing_ok=True)

    with HostServer(extra_args=["--dump", str(DUMP_FILE)]) as host:
        log("host started (--dump mode)")
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

            log("waiting 45s for load + rendering...")
            time.sleep(45)
            if not host.is_alive():
                log("ERROR: host died"); return

            log("recording 5s of main menu frames...")
            time.sleep(5)
            log("done — stopping")

    log(f"dump saved: {DUMP_FILE}")
    if DUMP_FILE.exists():
        sz = DUMP_FILE.stat().st_size
        log(f"dump size: {sz / 1024 / 1024:.1f} MB")
    else:
        log("ERROR: dump file not created!")

if __name__ == "__main__":
    run()
