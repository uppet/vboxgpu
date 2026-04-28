"""Run host with Vulkan validation layers to catch API misuse."""
import time, sys, os, subprocess
sys.path.insert(0, os.path.dirname(__file__))

from process import GuestProcess, _host_env
from input import find_hwnd
from pathlib import Path

GAME_EXE = Path(r"S:\bld\vboxgpu\tests\UltraKill\ULTRAKILL.exe")
HOST_EXE = Path(r"S:\bld\vboxgpu\build\host\Debug\vbox_host_server.exe")
HOST_CWD = Path(r"S:\bld\vboxgpu")
VAL_LOG  = Path(r"S:\bld\vboxgpu\validation_errors.txt")

def log(msg): print(f"[VAL] {msg}", flush=True)

def run():
    VAL_LOG.unlink(missing_ok=True)

    # Start host with validation layers enabled
    env = _host_env()
    env["VK_INSTANCE_LAYERS"] = "VK_LAYER_KHRONOS_validation"
    # Redirect stderr to capture validation messages
    host_cmd = f'"{HOST_EXE}"'
    log("starting host with validation layers...")
    host_proc = subprocess.Popen(
        host_cmd, cwd=str(HOST_CWD), env=env,
        stdout=subprocess.DEVNULL,
        stderr=open(str(VAL_LOG), "w"),
        creationflags=subprocess.CREATE_NEW_CONSOLE
    )
    time.sleep(2)

    with GuestProcess(GAME_EXE, extra_args=[
        "-screen-width", "1280", "-screen-height", "720",
        "-screen-fullscreen", "0",
    ]) as guest:
        try:
            hwnd = find_hwnd("ULTRAKILL", timeout=40.0)
        except TimeoutError as e:
            log(f"ERROR: {e}"); return
        log(f"game HWND={hwnd:#x}")
        log("waiting 40s for load + rendering with validation...")
        time.sleep(40)

    log("stopping host...")
    subprocess.run("taskkill /F /IM vbox_host_server.exe",
                    shell=True, capture_output=True)
    time.sleep(1)

    # Analyze validation output
    if VAL_LOG.exists():
        sz = VAL_LOG.stat().st_size
        log(f"validation log: {sz / 1024:.1f} KB")
        with open(str(VAL_LOG), "r", errors="replace") as f:
            content = f.read()
        # Count validation errors/warnings
        errors = content.count("Validation Error")
        warnings = content.count("Validation Warning")
        log(f"errors={errors} warnings={warnings}")
        # Show unique error types
        import re
        vuid_pattern = re.compile(r'\[VUID-[^\]]+\]')
        vuids = vuid_pattern.findall(content)
        unique_vuids = {}
        for v in vuids:
            unique_vuids[v] = unique_vuids.get(v, 0) + 1
        log("=== Unique validation issues ===")
        for vuid, count in sorted(unique_vuids.items(), key=lambda x: -x[1])[:20]:
            log(f"  {count:5d}x {vuid}")
    else:
        log("ERROR: no validation log")

if __name__ == "__main__":
    run()
