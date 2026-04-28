"""Record UK briefly to capture CopyImage ICD logs."""
import subprocess, time, sys, os
sys.path.insert(0, os.path.dirname(__file__))
from process import GuestProcess, _host_env
from input import find_hwnd
from pathlib import Path

HOST_EXE = Path(r"S:\bld\vboxgpu\build\host\Debug\vbox_host_server.exe")
GAME_EXE = Path(r"S:\bld\vboxgpu\tests\UltraKill\ULTRAKILL.exe")
HOST_CWD = Path(r"S:\bld\vboxgpu")
ICD_SRC  = Path(r"S:\bld\vboxgpu\build\guest_vk_icd\Debug\vbox_vulkan.dll")
ICD_DST  = Path(r"S:\bld\vboxgpu\tests\dx11_triangle\test_env\vbox_vulkan.dll")

def log(msg): print(f"[REC] {msg}", flush=True)

def run():
    subprocess.run("taskkill /F /IM vbox_host_server.exe", shell=True, capture_output=True)
    subprocess.run("taskkill /F /IM ULTRAKILL.exe", shell=True, capture_output=True)
    time.sleep(3)

    # Copy latest ICD
    import shutil
    shutil.copy2(str(ICD_SRC), str(ICD_DST))
    log("ICD copied")

    # Clear ICD log
    open(r"S:\bld\vboxgpu\icd_debug.log", "w").close()

    # Start host
    host_cmd = f'start "" /D "{HOST_CWD}" cmd /c ""{HOST_EXE}" 2>"{HOST_CWD}\\host_err.txt""'
    subprocess.Popen(host_cmd, shell=True, env=_host_env())
    time.sleep(2)

    with GuestProcess(GAME_EXE, extra_args=[
        "-screen-width", "1280", "-screen-height", "720", "-screen-fullscreen", "0",
    ]) as guest:
        try:
            hwnd = find_hwnd("ULTRAKILL", timeout=20.0)
        except TimeoutError:
            log("ERROR: no game window"); return
        log(f"game HWND={hwnd:#x}")
        log("recording 10 seconds...")
        time.sleep(10)

    subprocess.run("taskkill /F /IM vbox_host_server.exe", shell=True, capture_output=True)
    log("done")

if __name__ == "__main__":
    run()
