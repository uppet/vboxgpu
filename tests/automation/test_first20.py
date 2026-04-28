"""Record first ~20 frames of UK, replay, save each frame as image."""
import subprocess, time, os, sys
sys.path.insert(0, os.path.dirname(__file__))
from process import GuestProcess, _host_env
from input import find_hwnd
from pathlib import Path

HOST_EXE  = Path(r"S:\bld\vboxgpu\build\host\Debug\vbox_host_server.exe")
GAME_EXE  = Path(r"S:\bld\vboxgpu\tests\UltraKill\ULTRAKILL.exe")
DUMP_DIR  = Path(r"S:\bld\vboxgpu\dumps\first20")
FRAME_DIR = Path(r"S:\bld\vboxgpu\dumps\first20_frames")
HOST_CWD  = Path(r"S:\bld\vboxgpu")

def log(msg): print(f"[F20] {msg}", flush=True)

def run():
    # Clean
    subprocess.run("taskkill /F /IM vbox_host_server.exe", shell=True, capture_output=True)
    subprocess.run("taskkill /F /IM ULTRAKILL.exe", shell=True, capture_output=True)
    time.sleep(3)

    DUMP_DIR.mkdir(parents=True, exist_ok=True)
    FRAME_DIR.mkdir(parents=True, exist_ok=True)
    # Remove old dumps
    for f in DUMP_DIR.glob("*.bin"):
        f.unlink()
    for f in FRAME_DIR.glob("*.bmp"):
        f.unlink()

    # Step 1: Record ~5 seconds (includes logo + first frames)
    log("=== Step 1: Record ===")
    host_cmd = f'start "" /D "{HOST_CWD}" cmd /c ""{HOST_EXE}" --dump "{DUMP_DIR}" 2>"{DUMP_DIR}\\host_rec.txt""'
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
        log("recording 8 seconds...")
        time.sleep(8)

    subprocess.run("taskkill /F /IM vbox_host_server.exe", shell=True, capture_output=True)
    time.sleep(2)

    # Check dump
    dumps = list(DUMP_DIR.glob("session_*.bin"))
    if not dumps:
        log("ERROR: no dump file created!")
        log(f"  files in {DUMP_DIR}: {list(DUMP_DIR.iterdir())}")
        return
    dump_file = dumps[0]
    sz = dump_file.stat().st_size
    log(f"dump: {dump_file.name} ({sz / 1024:.0f} KB)")

    # Step 2: Replay with --save-frames
    log("=== Step 2: Replay + save frames ===")
    replay_cmd = f'"{HOST_EXE}" --replay "{dump_file}" --save-frames "{FRAME_DIR}"'
    log(f"cmd: {replay_cmd}")
    result = subprocess.run(replay_cmd, cwd=str(HOST_CWD), env=_host_env(),
                           capture_output=True, text=True, timeout=60)
    log(f"replay exit={result.returncode}")
    if result.stderr:
        for line in result.stderr.strip().splitlines()[-10:]:
            log(f"  {line}")

    # Check frames
    frames = sorted(FRAME_DIR.glob("*.bmp"))
    log(f"saved {len(frames)} frames")
    for f in frames[:5]:
        log(f"  {f.name} ({f.stat().st_size / 1024:.0f} KB)")

    log("=== Done ===")
    log(f"Frames in: {FRAME_DIR}")
    log(f"Host recording log: {DUMP_DIR / 'host_rec.txt'}")

if __name__ == "__main__":
    run()
