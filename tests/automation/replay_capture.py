"""Replay dump and capture frames externally via WGC."""
import subprocess, time, sys, os
sys.path.insert(0, os.path.dirname(__file__))
from capture import capture_window
from pathlib import Path

HOST_EXE  = Path(r"S:\bld\vboxgpu\build\host\Debug\vbox_host_server.exe")
DUMP_FILE = Path(r"S:\bld\vboxgpu\dumps\first20\session_0.bin")
OUT_DIR   = Path(r"S:\bld\vboxgpu\dumps\first20_frames")
CAPTURE   = Path(r"S:\bld\vboxgpu\build\tools\capture_helper\Debug\capture_helper.exe")

def log(msg): print(f"[RC] {msg}", flush=True)

def run():
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    # Start replay in background (no --save-frames to avoid crash)
    log("Starting replay...")
    proc = subprocess.Popen(
        f'start "" /D "S:\\bld\\vboxgpu" "{HOST_EXE}" --replay "{DUMP_FILE}"',
        shell=True
    )

    # Wait for window to appear and rendering to start
    time.sleep(5)

    # Capture multiple frames
    for i in range(10):
        dst = OUT_DIR / f"replay_ext_{i:02d}.png"
        try:
            img = capture_window("Client")
            img.save(str(dst))
            import numpy as np
            mean = float(np.array(img).mean())
            log(f"frame {i}: {img.size} mean={mean:.1f}")
        except Exception as e:
            log(f"frame {i}: FAILED {e}")
        time.sleep(1.5)

    # Kill replay
    subprocess.run("taskkill /F /IM vbox_host_server.exe", shell=True, capture_output=True)
    log("Done — check dumps/first20_frames/")

if __name__ == "__main__":
    run()
