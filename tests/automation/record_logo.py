"""Record logo phase dump, then replay with frame saves."""
import subprocess, time, sys, os
sys.path.insert(0, os.path.dirname(__file__))
from process import HostServer, GuestProcess, _host_env, HOST_SERVER_EXE, HOST_SERVER_CWD
from pathlib import Path
import shutil

GAME_EXE = Path(r"S:\bld\vboxgpu\tests\UltraKill\ULTRAKILL.exe")
DUMP_DIR = Path(r"S:\bld\vboxgpu\dumps\logo2")
FRAMES_DIR = DUMP_DIR / "frames"

def log(msg): print(f"[REC] {msg}", flush=True)

def run():
    # Clean
    subprocess.run("taskkill /F /IM vbox_host_server.exe", shell=True, capture_output=True)
    subprocess.run("taskkill /F /IM ULTRAKILL.exe", shell=True, capture_output=True)
    time.sleep(3)
    DUMP_DIR.mkdir(parents=True, exist_ok=True)
    for f in DUMP_DIR.glob("*.bin"): f.unlink()

    # Record: host with --dump
    host_cmd = f'start "" /D "{HOST_SERVER_CWD}" cmd /c ""{HOST_SERVER_EXE}" --dump "{DUMP_DIR}" 2>"{DUMP_DIR}\\host_rec.txt""'
    subprocess.Popen(host_cmd, shell=True, env=_host_env())
    time.sleep(2)
    log("Host started with --dump")

    with GuestProcess(GAME_EXE, extra_args=[
        "-screen-width", "1280", "-screen-height", "720", "-screen-fullscreen", "0",
    ]) as guest:
        log("Guest started, recording 10s (logo phase)...")
        time.sleep(10)

    subprocess.run("taskkill /F /IM vbox_host_server.exe", shell=True, capture_output=True)
    time.sleep(2)

    dump = DUMP_DIR / "session_0.bin"
    if dump.exists():
        log(f"Dump: {dump.name} ({dump.stat().st_size / 1024 / 1024:.1f} MB)")
    else:
        log("ERROR: no dump file!")
        return

    # Replay with --save-frames
    if FRAMES_DIR.exists():
        shutil.rmtree(FRAMES_DIR)
    FRAMES_DIR.mkdir()

    log("Replaying with --save-frames...")
    replay_cmd = [str(HOST_SERVER_EXE), "--replay", str(dump), "--save-frames", str(FRAMES_DIR)]
    result = subprocess.run(replay_cmd, capture_output=True, text=True, timeout=120,
                          cwd=str(HOST_SERVER_CWD), env=_host_env())
    log(f"Replay exit={result.returncode}")

    frames = sorted(FRAMES_DIR.glob("*.bmp"))
    log(f"Saved {len(frames)} frames")
    if frames:
        log(f"First: {frames[0].name}, Last: {frames[-1].name}")
        # Check if first few frames are black
        for f in frames[:3]:
            sz = f.stat().st_size
            log(f"  {f.name}: {sz} bytes")

if __name__ == "__main__":
    run()
