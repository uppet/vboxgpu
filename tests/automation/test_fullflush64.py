"""Test with 64MB full flush to see if glitch disappears."""
import subprocess, time, sys, os
sys.path.insert(0, os.path.dirname(__file__))
from process import GuestProcess, _host_env
from capture import capture_window
from input import find_hwnd
from pathlib import Path
import numpy as np

HOST_EXE = Path(r"S:\bld\vboxgpu\build\host\Debug\vbox_host_server.exe")
GAME_EXE = Path(r"S:\bld\vboxgpu\tests\UltraKill\ULTRAKILL.exe")
HOST_CWD = Path(r"S:\bld\vboxgpu")
ICD_SRC  = Path(r"S:\bld\vboxgpu\build\guest_vk_icd\Debug\vbox_vulkan.dll")
ICD_DST  = Path(r"S:\bld\vboxgpu\tests\dx11_triangle\test_env\vbox_vulkan.dll")
OUT_DIR  = Path(r"S:\bld\vboxgpu\automation_out\fullflush64")

def log(msg): print(f"[FF64] {msg}", flush=True)

def run():
    subprocess.run("taskkill /F /IM vbox_host_server.exe", shell=True, capture_output=True)
    subprocess.run("taskkill /F /IM ULTRAKILL.exe", shell=True, capture_output=True)
    time.sleep(3)

    import shutil
    shutil.copy2(str(ICD_SRC), str(ICD_DST))
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    host_cmd = f'start "" /D "{HOST_CWD}" cmd /c ""{HOST_EXE}" 2>"{HOST_CWD}\\host_err.txt""'
    subprocess.Popen(host_cmd, shell=True, env=_host_env())
    time.sleep(2)

    with GuestProcess(GAME_EXE, extra_args=[
        "-screen-width", "1280", "-screen-height", "720", "-screen-fullscreen", "0",
    ]) as guest:
        try:
            hwnd = find_hwnd("ULTRAKILL", timeout=30.0)
        except TimeoutError:
            log("ERROR: no window"); return
        log(f"game HWND={hwnd:#x}")
        log("waiting 35s for load...")
        time.sleep(35)

        for i in range(5):
            dst = OUT_DIR / f"frame_{i:02d}.png"
            try:
                img = capture_window("ULTRAKILL")
                img.save(str(dst))
                mean = float(np.array(img).mean())
                log(f"frame {i}: mean={mean:.1f}")
            except Exception as e:
                log(f"frame {i}: FAILED {e}")
            time.sleep(2)

    subprocess.run("taskkill /F /IM vbox_host_server.exe", shell=True, capture_output=True)
    log("done — check automation_out/fullflush64/")

if __name__ == "__main__":
    run()
