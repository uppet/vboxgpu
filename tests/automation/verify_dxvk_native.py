"""Test DXVK native rendering (no ICD bridge) — verify if glitch is DXVK-side."""
import time, sys, os, subprocess
sys.path.insert(0, os.path.dirname(__file__))

from capture import capture_window
from pathlib import Path
import numpy as np

GAME_EXE = Path(r"S:\bld\vboxgpu\tests\UltraKill\ULTRAKILL.exe")
GAME_DIR = GAME_EXE.parent
OUT_DIR  = Path(r"S:\bld\vboxgpu\automation_out\verify_dxvk_native")

def log(msg): print(f"[NATIVE] {msg}", flush=True)

def snap(label):
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    dst = OUT_DIR / f"{label}.png"
    try:
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

    # Start UltraKill WITHOUT VK_ICD_FILENAMES — DXVK uses system Vulkan driver.
    # Also clear VK_LOADER_LAYERS_DISABLE to allow validation if present.
    env = os.environ.copy()
    env.pop("VK_ICD_FILENAMES", None)
    env.pop("VK_LOADER_LAYERS_DISABLE", None)

    log(f"starting UK natively (no ICD bridge)...")
    proc = subprocess.Popen(
        [str(GAME_EXE), "-screen-width", "1280", "-screen-height", "720",
         "-screen-fullscreen", "0"],
        cwd=str(GAME_DIR), env=env
    )
    log(f"PID={proc.pid}")

    log("waiting 30s for shader compile + load...")
    time.sleep(30)

    if proc.poll() is not None:
        log(f"ERROR: game exited with code {proc.returncode}")
        return

    for i in range(8):
        snap(f"frame_{i:02d}")
        time.sleep(2)

    log("done — check screenshots in automation_out/verify_dxvk_native/")
    log("killing game...")
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()

if __name__ == "__main__":
    run()
