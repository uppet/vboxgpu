"""Baseline test: start host+UK, wait for rendering, capture host window screenshot."""
import subprocess, time, sys, os
sys.path.insert(0, os.path.dirname(__file__))
from process import HostServer, GuestProcess
from pathlib import Path

GAME_EXE = Path(r"S:\bld\vboxgpu\tests\UltraKill\ULTRAKILL.exe")
CAPTURE  = Path(r"S:\bld\vboxgpu\build\tools\capture_helper\Debug\capture_helper.exe")
OUT_DIR  = Path(r"S:\bld\vboxgpu")

def log(msg): print(f"[BL] {msg}", flush=True)

def run():
    host = HostServer()
    host.ensure_running()
    log(f"Host running, pid={host._pid}")
    time.sleep(2)

    with GuestProcess(GAME_EXE, extra_args=[
        "-screen-width", "1280", "-screen-height", "720", "-screen-fullscreen", "0",
    ]) as guest:
        log("Guest started, waiting 15s for rendering...")
        time.sleep(15)

        # Capture host render window
        for title, name in [("Host Render", "bl_host_render.png"), ("VBox GPU", "bl_host_dash.png")]:
            out = OUT_DIR / name
            r = subprocess.run([str(CAPTURE), title, str(out)],
                               capture_output=True, text=True, timeout=10)
            if r.returncode == 0:
                log(f"Captured: {out} ({r.stdout.strip()})")
            else:
                log(f"Capture '{title}' failed: {r.stderr.strip()}")

        log("Waiting 5 more seconds...")
        time.sleep(5)

    host.stop()
    log("Done.")

if __name__ == "__main__":
    run()
