"""Check if native DXVK also creates 1920x1080 RT at 1280x720 window."""
import subprocess, time, os
from pathlib import Path

GAME_EXE = Path(r"S:\bld\vboxgpu\tests\UltraKill\ULTRAKILL.exe")
GAME_DIR = GAME_EXE.parent
LOG_FILE = Path(r"S:\bld\vboxgpu\dxvk_native_log.txt")

env = os.environ.copy()
env.pop("VK_ICD_FILENAMES", None)
env.pop("VK_LOADER_LAYERS_DISABLE", None)
env["DXVK_LOG_LEVEL"] = "info"
env["DXVK_LOG_PATH"] = str(GAME_DIR)

print("Starting UK natively with DXVK logging...")
proc = subprocess.Popen(
    [str(GAME_EXE), "-screen-width", "1280", "-screen-height", "720", "-screen-fullscreen", "0"],
    cwd=str(GAME_DIR), env=env
)
print(f"PID={proc.pid}, waiting 30s...")
time.sleep(30)
proc.terminate()
try: proc.wait(5)
except: proc.kill()

# Check DXVK log for image creation
log_candidates = list(GAME_DIR.glob("*.log"))
print(f"Log files: {[f.name for f in log_candidates]}")
for lf in log_candidates:
    if lf.stat().st_size > 0:
        with open(lf, "r", errors="replace") as f:
            content = f.read()
        if "1920" in content or "createImage" in content.lower():
            print(f"\n=== {lf.name} (1920 mentions) ===")
            for line in content.splitlines():
                if "1920" in line or "createImage" in line.lower() or "render target" in line.lower():
                    print(f"  {line.strip()}")
print("Done")
