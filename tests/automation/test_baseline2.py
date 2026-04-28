"""Baseline test v2: start host+UK, capture using dbg_capture_request flag file."""
import subprocess, time, sys, os
sys.path.insert(0, os.path.dirname(__file__))
from process import HostServer, GuestProcess
from pathlib import Path

GAME_EXE = Path(r"S:\bld\vboxgpu\tests\UltraKill\ULTRAKILL.exe")
CAPTURE  = Path(r"S:\bld\vboxgpu\build\tools\capture_helper\Debug\capture_helper.exe")
OUT_DIR  = Path(r"S:\bld\vboxgpu")

def log(msg): print(f"[BL2] {msg}", flush=True)

def capture_any(name, title):
    out = OUT_DIR / name
    r = subprocess.run([str(CAPTURE), title, str(out)],
                       capture_output=True, text=True, timeout=10)
    if r.returncode == 0:
        log(f"Captured {name}: {r.stdout.strip()}")
        return True
    log(f"Capture '{title}' -> {name} failed: {r.stdout.strip()}")
    return False

def run():
    host = HostServer()
    host.ensure_running()
    log(f"Host running")
    time.sleep(2)

    with GuestProcess(GAME_EXE, extra_args=[
        "-screen-width", "1280", "-screen-height", "720", "-screen-fullscreen", "0",
    ]) as guest:
        log("Guest started, waiting 20s...")
        time.sleep(20)

        # Try various window titles
        for title in ["Client", "Bridge - Client", "ULTRAKILL", "VBox"]:
            capture_any(f"bl2_{title.replace(' ','_')}.png", title)

        # Check host log for frames
        try:
            import re
            log_text = Path(r"S:\bld\vboxgpu\auto_host_err.txt").read_text(errors='ignore')
            frames = re.findall(r'frame=(\d+)', log_text)
            if frames:
                max_frame = max(int(f) for f in frames)
                log(f"Host rendered up to frame={max_frame}")
            else:
                log("Host: NO frames rendered!")
            present_count = log_text.count("present done")
            log(f"Host: {present_count} presents")
        except Exception as e:
            log(f"Log check error: {e}")

        log("Keeping alive 5 more seconds...")
        time.sleep(5)

    log("Guest stopped")
    # Don't stop host — leave it for inspection
    log("Done (host still running).")

if __name__ == "__main__":
    run()
