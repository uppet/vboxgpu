"""Record a new dump with format blocking enabled."""
import subprocess, time, sys, os
sys.path.insert(0, os.path.dirname(__file__))
from process import GuestProcess, _host_env
from input import find_hwnd
from pathlib import Path
import shutil

HOST_EXE = Path(r"S:\bld\vboxgpu\build\host\Debug\vbox_host_server.exe")
GAME_EXE = Path(r"S:\bld\vboxgpu\tests\UltraKill\ULTRAKILL.exe")
HOST_CWD = Path(r"S:\bld\vboxgpu")
ICD_SRC  = Path(r"S:\bld\vboxgpu\build\guest_vk_icd\Debug\vbox_vulkan.dll")
ICD_DST  = Path(r"S:\bld\vboxgpu\tests\dx11_triangle\test_env\vbox_vulkan.dll")
DUMP_DIR = Path(r"S:\bld\vboxgpu\dumps\fmtblock")

def log(msg): print(f"[RD] {msg}", flush=True)

def run():
    subprocess.run("taskkill /F /IM vbox_host_server.exe", shell=True, capture_output=True)
    subprocess.run("taskkill /F /IM ULTRAKILL.exe", shell=True, capture_output=True)
    time.sleep(3)
    shutil.copy2(str(ICD_SRC), str(ICD_DST))
    DUMP_DIR.mkdir(parents=True, exist_ok=True)
    for f in DUMP_DIR.glob("*.bin"): f.unlink()

    host_cmd = f'start "" /D "{HOST_CWD}" cmd /c ""{HOST_EXE}" --dump "{DUMP_DIR}" 2>"{DUMP_DIR}\\host_rec.txt""'
    subprocess.Popen(host_cmd, shell=True, env=_host_env())
    time.sleep(2)

    with GuestProcess(GAME_EXE, extra_args=[
        "-screen-width", "1280", "-screen-height", "720", "-screen-fullscreen", "0",
    ]) as guest:
        try:
            hwnd = find_hwnd("ULTRAKILL", timeout=20.0)
        except TimeoutError:
            log("ERROR: no window"); return
        log(f"HWND={hwnd:#x}, recording 8s...")
        time.sleep(8)

    subprocess.run("taskkill /F /IM vbox_host_server.exe", shell=True, capture_output=True)
    dumps = list(DUMP_DIR.glob("session_*.bin"))
    if dumps:
        log(f"dump: {dumps[0].name} ({dumps[0].stat().st_size / 1024:.0f} KB)")
    else:
        log("ERROR: no dump")

if __name__ == "__main__":
    run()
