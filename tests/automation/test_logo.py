"""Test logo video: capture at 5s (during logo) and 20s (menu)."""
import subprocess, time, sys, os
sys.path.insert(0, os.path.dirname(__file__))
from process import HostServer, GuestProcess
from pathlib import Path

GAME_EXE = Path(r"S:\bld\vboxgpu\tests\UltraKill\ULTRAKILL.exe")
CAPTURE  = Path(r"S:\bld\vboxgpu\build\tools\capture_helper\Debug\capture_helper.exe")
OUT_DIR  = Path(r"S:\bld\vboxgpu")

def log(msg): print(f"[LOGO] {msg}", flush=True)
def cap(name):
    out = OUT_DIR / name
    r = subprocess.run([str(CAPTURE), "ULTRAKILL", str(out)],
                       capture_output=True, text=True, timeout=10)
    log(f"{name}: {'OK' if r.returncode == 0 else 'FAIL'} {r.stdout.strip()}")

def run():
    host = HostServer()
    host.ensure_running()
    log("Host running")

    with GuestProcess(GAME_EXE, extra_args=[
        "-screen-width", "1280", "-screen-height", "720", "-screen-fullscreen", "0",
    ]) as guest:
        log("Guest started")
        time.sleep(5)
        cap("logo_5s.png")
        time.sleep(3)
        cap("logo_8s.png")
        time.sleep(12)
        cap("logo_20s.png")
        time.sleep(3)

    host.stop()
    log("Done.")

if __name__ == "__main__":
    run()
