"""Record UltraKill menu screen dump, then replay with --save-frames for glitch analysis."""
import subprocess, time, sys, os
sys.path.insert(0, os.path.dirname(__file__))
from process import GuestProcess, _host_env
from input import find_hwnd
from pathlib import Path

HOST_EXE = Path(r"S:\bld\vboxgpu\build\host\Debug\vbox_host_server.exe")
GAME_EXE = Path(r"S:\bld\vboxgpu\tests\UltraKill\ULTRAKILL.exe")
HOST_CWD = Path(r"S:\bld\vboxgpu")
DUMP_DIR = Path(r"S:\bld\vboxgpu\dumps\menu")
FRAMES_DIR = DUMP_DIR / "frames"

def log(msg): print(f"[MenuDump] {msg}", flush=True)

def kill_existing():
    subprocess.run("taskkill /F /IM vbox_host_server.exe", shell=True, capture_output=True)
    subprocess.run("taskkill /F /IM ULTRAKILL.exe", shell=True, capture_output=True)
    time.sleep(3)

def record():
    """Phase 1: Record command stream dump."""
    kill_existing()

    # Prepare dump directory
    DUMP_DIR.mkdir(parents=True, exist_ok=True)
    for f in DUMP_DIR.glob("*.bin"):
        f.unlink()

    # Start host with --dump (using start command for independent console)
    host_err = DUMP_DIR / "host_rec_err.txt"
    host_cmd = (
        f'start "" /D "{HOST_CWD}" cmd /c "'
        f'"{HOST_EXE}" --dump "{DUMP_DIR}" 2>"{host_err}""'
    )
    log(f"starting host: {host_cmd}")
    subprocess.Popen(host_cmd, shell=True, env=_host_env())
    time.sleep(2)

    # Start UltraKill at 1280x720 windowed
    with GuestProcess(GAME_EXE, extra_args=[
        "-screen-width", "1280", "-screen-height", "720", "-screen-fullscreen", "0",
    ]) as guest:
        log("waiting for game window...")
        try:
            hwnd = find_hwnd("ULTRAKILL", timeout=30.0)
        except TimeoutError:
            log("ERROR: game window not found")
            subprocess.run("taskkill /F /IM vbox_host_server.exe", shell=True, capture_output=True)
            return None
        log(f"HWND={hwnd:#x}")

        log("recording 8s of menu screen...")
        time.sleep(8)

        if not subprocess.run(
            "tasklist /FI \"IMAGENAME eq vbox_host_server.exe\" /NH",
            shell=True, capture_output=True, text=True
        ).stdout.strip():
            log("WARNING: host may have died")

    # Kill host to finalize dump
    log("stopping host...")
    subprocess.run("taskkill /F /IM vbox_host_server.exe", shell=True, capture_output=True)
    time.sleep(1)

    # Report
    dumps = sorted(DUMP_DIR.glob("session_*.bin"))
    if dumps:
        for d in dumps:
            sz = d.stat().st_size
            log(f"dump: {d.name} ({sz / 1024:.0f} KB, {sz / 1024 / 1024:.1f} MB)")
        return dumps[0]
    else:
        log("ERROR: no dump files created!")
        # Check host error log
        if host_err.exists():
            log(f"host stderr tail:\n{host_err.read_text(errors='ignore')[-500:]}")
        return None

def replay(dump_file):
    """Phase 2: Replay dump with --save-frames."""
    kill_existing()

    FRAMES_DIR.mkdir(parents=True, exist_ok=True)
    # Clean old frames
    for f in FRAMES_DIR.glob("*.bmp"):
        f.unlink()

    host_err = DUMP_DIR / "host_replay_err.txt"
    # Replay is synchronous (exits when done), run directly
    log(f"replaying {dump_file.name} with --save-frames...")
    cmd = [
        str(HOST_EXE),
        "--replay", str(dump_file),
        "--save-frames", str(FRAMES_DIR),
    ]
    log(f"cmd: {' '.join(cmd)}")
    result = subprocess.run(
        cmd,
        cwd=str(HOST_CWD),
        env=_host_env(),
        capture_output=True,
        text=True,
        timeout=120,
    )
    # Save logs
    host_err.write_text(result.stderr, encoding='utf-8')
    log(f"replay exit code: {result.returncode}")
    if result.stderr:
        # Show last 1000 chars of stderr
        tail = result.stderr[-1000:]
        log(f"replay stderr tail:\n{tail}")

    # Report frames
    frames = sorted(FRAMES_DIR.glob("*.bmp"))
    log(f"frames saved: {len(frames)}")
    for f in frames[-10:]:  # show last 10
        log(f"  {f.name} ({f.stat().st_size / 1024:.0f} KB)")
    return frames

def main():
    log("=== Phase 1: Record ===")
    dump_file = record()
    if not dump_file:
        log("Recording failed, aborting.")
        return

    log("")
    log("=== Phase 2: Replay with frame saves ===")
    frames = replay(dump_file)
    if not frames:
        log("No frames saved.")
    else:
        log(f"\nDone. {len(frames)} frames in {FRAMES_DIR}")

if __name__ == "__main__":
    main()
