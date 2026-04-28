"""
sort_the_court.py — Automation script for Sort the Court!

Flow:
  1. Start host_server + game
  2. Wait for game window (Unity logo / splash)
  3. Skip splash screens with Space
  4. Wait for main menu (stable frame)
  5. Press Space to start game
  6. Play N rounds: alternate Yes (left) / No (right) clicks

Window title: "Sort the Court!" (matched by substr "Sort")
Game exe: tests/SortTheCourt/SortTheCourt.exe
"""

import time
from pathlib import Path

import sys, os
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from capture import (
    capture_window, is_black, frames_differ,
    wait_for_nonblack, wait_for_stable_frame,
)
from input import find_hwnd, focus_window, click_at, press_space, press_enter
from process import HostServer, GuestProcess
from PIL import Image

GAME_EXE      = Path(r"S:\bld\vboxgpu\tests\SortTheCourt\SortTheCourt.exe")
TITLE_SUBSTR  = "Sort the Court"  # game window — used for interaction and WGC capture
HOST_DBG_BMP  = Path(r"S:\bld\vboxgpu\dbg_frame5.bmp")   # host server built-in screenshot

# Approximate relative positions in the game window (0.0-1.0)
# The Yes (heart) button is on the left, No (X) is on the right
YES_POS = (0.25, 0.78)
NO_POS  = (0.75, 0.78)
# "Start game" click — roughly center of screen
START_POS = (0.50, 0.55)


def _skip_splashes(hwnd: int, count: int = 4, interval: float = 1.5):
    """Mash Space several times to skip Unity logo and game splash screens."""
    for _ in range(count):
        press_space(hwnd)
        time.sleep(interval)


def run(rounds: int = 5, verbose: bool = True) -> dict:
    """
    Run Sort the Court for the given number of rounds.
    Returns a dict with 'rounds_played', 'screenshots' (list of PIL Images), 'error'.
    """
    result = {"rounds_played": 0, "screenshots": [], "error": None}

    def log(msg):
        if verbose:
            print(f"[SortTheCourt] {msg}")

    # Remove stale host screenshot so we can detect a fresh write later
    try:
        HOST_DBG_BMP.unlink(missing_ok=True)
    except Exception:
        pass

    with HostServer() as host:
        log("host_server started")

        with GuestProcess(GAME_EXE) as guest:
            log("game started, waiting for window...")

            # 1. Find game window
            try:
                hwnd = find_hwnd(TITLE_SUBSTR, timeout=30.0)
            except TimeoutError as e:
                result["error"] = str(e)
                return result
            log(f"window found: HWND={hwnd:#x}")

            # 2. Wait for game window to show any non-black content (Unity splash or title)
            # WGC captures the game window (GDI StretchDIBits framebuffer return via DWM).
            # Host server Vulkan window is not used for WGC (WGC unreliable for Vulkan windows).
            try:
                img = wait_for_nonblack(TITLE_SUBSTR, timeout=40.0)
            except TimeoutError as e:
                result["error"] = str(e)
                return result
            log("got first non-black frame")
            result["screenshots"].append(img)

            # 3. Let Unity splash and main menu load.
            # SortTheCourt: Unity logo ~3s, title screen appears and waits for input.
            log("waiting for menu to appear (15s)...")
            time.sleep(15.0)

            # Confirm host rendering via built-in dbg_frame5.bmp (written at frame 5)
            # Delete stale file first so we can detect a fresh write.
            try:
                if HOST_DBG_BMP.exists():
                    host_bmp = Image.open(str(HOST_DBG_BMP)).copy()
                    if not is_black(host_bmp):
                        result["screenshots"].append(host_bmp)
                        log("host render confirmed via dbg_frame5.bmp")
                    else:
                        log("host dbg_frame5.bmp is black — rendering may not have started")
                else:
                    log("host dbg_frame5.bmp not found — skipping host render check")
            except Exception as e:
                log(f"host bmp read warning: {e}")

            # 4. Skip any remaining splash and start the game
            log("clicking through to game start...")
            press_space(hwnd)
            time.sleep(1.0)
            click_at(hwnd, *START_POS)
            time.sleep(1.0)
            press_space(hwnd)
            time.sleep(1.0)
            press_enter(hwnd)

            log("waiting for game scene to load (6s)...")
            time.sleep(6.0)

            try:
                game_frame = capture_window(TITLE_SUBSTR)
                result["screenshots"].append(game_frame)
                if is_black(game_frame):
                    log("WARNING: game scene frame is black")
            except Exception:
                pass
            log("game scene loaded")

            # 6. Play rounds
            for i in range(rounds):
                log(f"round {i+1}/{rounds}: waiting for question...")
                time.sleep(2.5)  # let dragon king speak

                # Alternate yes/no
                if i % 2 == 0:
                    log(f"  clicking YES")
                    click_at(hwnd, *YES_POS)
                else:
                    log(f"  clicking NO")
                    click_at(hwnd, *NO_POS)

                time.sleep(1.5)  # let result animation play

                try:
                    frame = capture_window(TITLE_SUBSTR)
                    result["screenshots"].append(frame)
                    result["rounds_played"] += 1
                    if is_black(frame):
                        log(f"  WARNING: captured frame is black")
                except Exception as e:
                    log(f"  capture failed: {e}")

                if not guest.is_alive():
                    log("game process exited unexpectedly")
                    result["error"] = "game exited"
                    break

            log(f"done. {result['rounds_played']} rounds played.")

    return result


if __name__ == "__main__":
    r = run(rounds=5, verbose=True)
    print(f"\nResult: {r['rounds_played']} rounds, error={r['error']}")
    print(f"Screenshots captured: {len(r['screenshots'])}")
