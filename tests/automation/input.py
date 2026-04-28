"""
input.py — Window-targeted keyboard and mouse input via pywin32.

Uses SendInput for mouse (works reliably with Unity games).
Uses PostMessage for keyboard (avoids focus issues for some keys).
"""

import ctypes
import time
import win32api
import win32con
import win32gui


def find_hwnd(title_substr: str, timeout: float = 20.0) -> int:
    """Find a visible window whose title contains title_substr.
    Returns HWND. Raises TimeoutError if not found within timeout.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = []

        def _cb(hwnd, _):
            if win32gui.IsWindowVisible(hwnd):
                title = win32gui.GetWindowText(hwnd)
                if title_substr.lower() in title.lower():
                    result.append(hwnd)
            return True

        win32gui.EnumWindows(_cb, None)
        if result:
            return result[0]
        time.sleep(0.3)
    raise TimeoutError(f"Window with '{title_substr}' not found after {timeout}s")


def focus_window(hwnd: int):
    """Bring the window to foreground using AttachThreadInput to bypass foreground lock."""
    try:
        # AttachThreadInput forces our thread's input queue to be associated with the
        # target window's thread, which allows SetForegroundWindow and SendInput to
        # reach the correct window even when our process lacks foreground rights.
        # (delegate-runner subprocesses often fail SetForegroundWindow silently.)
        target_tid = ctypes.windll.user32.GetWindowThreadProcessId(hwnd, None)
        my_tid = ctypes.windll.kernel32.GetCurrentThreadId()
        if target_tid != my_tid:
            ctypes.windll.user32.AttachThreadInput(target_tid, my_tid, True)
        win32gui.SetForegroundWindow(hwnd)
        time.sleep(0.05)
        if target_tid != my_tid:
            ctypes.windll.user32.AttachThreadInput(target_tid, my_tid, False)
    except Exception:
        pass
    time.sleep(0.05)


def _client_to_screen(hwnd: int, cx: float, cy: float) -> tuple[int, int]:
    """Convert relative client coordinates (0.0-1.0) to absolute screen coordinates."""
    left, top, right, bottom = win32gui.GetClientRect(hwnd)
    # GetClientRect returns (0, 0, w, h) — need to map to screen
    pt = win32gui.ClientToScreen(hwnd, (0, 0))
    w = right - left
    h = bottom - top
    sx = pt[0] + int(w * cx)
    sy = pt[1] + int(h * cy)
    return sx, sy


def move_to(hwnd: int, cx: float, cy: float, delay: float = 0.05):
    """Move physical cursor to (cx, cy) without clicking. Sends WM_MOUSEMOVE for hover."""
    focus_window(hwnd)
    left, top, right, bottom = win32gui.GetClientRect(hwnd)
    w = right - left
    h = bottom - top
    lx = int(w * cx)
    ly = int(h * cy)
    sx, sy = _client_to_screen(hwnd, cx, cy)
    ctypes.windll.user32.SetCursorPos(sx, sy)
    lparam = (ly << 16) | (lx & 0xFFFF)
    win32gui.PostMessage(hwnd, win32con.WM_MOUSEMOVE, 0, lparam)
    time.sleep(delay)


def glide_to(hwnd: int, cx: float, cy: float,
             from_cx: float = 0.50, from_cy: float = 0.50,
             steps: int = 20, step_delay: float = 0.03):
    """Smoothly move cursor from (from_cx, from_cy) to (cx, cy) via multiple WM_MOUSEMOVE events.

    ULTRAKILL and some Unity games require continuous mouse movement (not a single jump)
    to trigger hover state detection. This function glides the cursor over N steps so the
    game's input polling sees a real movement trajectory.
    """
    focus_window(hwnd)
    left, top, right, bottom = win32gui.GetClientRect(hwnd)
    w = right - left
    h = bottom - top
    for i in range(steps + 1):
        t = i / steps
        ix = from_cx + (cx - from_cx) * t
        iy = from_cy + (cy - from_cy) * t
        lx = int(w * ix)
        ly = int(h * iy)
        sx, sy = _client_to_screen(hwnd, ix, iy)
        ctypes.windll.user32.SetCursorPos(sx, sy)
        lparam = (ly << 16) | (lx & 0xFFFF)
        win32gui.PostMessage(hwnd, win32con.WM_MOUSEMOVE, 0, lparam)
        time.sleep(step_delay)
    # Pause at target so hover animation can settle
    time.sleep(0.3)


def click_at(hwnd: int, cx: float, cy: float, delay: float = 0.05):
    """Left-click at (cx, cy) relative to window client area (0.0-1.0 each axis).

    Strategy:
      1. Move physical cursor to target screen position (SetCursorPos).
      2. Send WM_MOUSEMOVE so Unity UI sees hover.
      3. Send SendInput left-click (works for UnityEngine.UI menus).
      4. Also send PostMessage WM_LBUTTONDOWN/UP (works for in-game Unity input).
    """
    focus_window(hwnd)
    left, top, right, bottom = win32gui.GetClientRect(hwnd)
    w = right - left
    h = bottom - top
    lx = int(w * cx)
    ly = int(h * cy)

    # Map client coords → screen coords
    sx, sy = _client_to_screen(hwnd, cx, cy)

    # Move physical cursor
    ctypes.windll.user32.SetCursorPos(sx, sy)
    time.sleep(0.05)

    # Send hover so Unity UI sees cursor enter
    lparam = (ly << 16) | (lx & 0xFFFF)
    win32gui.PostMessage(hwnd, win32con.WM_MOUSEMOVE, win32con.MK_LBUTTON, lparam)
    time.sleep(0.05)

    # SendInput click (needed for UnityEngine.UI buttons)
    _sendinput_click()

    # Also PostMessage click (for in-game Unity input system)
    win32gui.PostMessage(hwnd, win32con.WM_LBUTTONDOWN, win32con.MK_LBUTTON, lparam)
    time.sleep(delay)
    win32gui.PostMessage(hwnd, win32con.WM_LBUTTONUP, 0, lparam)


def _sendinput_click(delay: float = 0.05):
    """Send a left mouse button click via SendInput (works for UI overlays)."""
    MOUSEEVENTF_LEFTDOWN = 0x0002
    MOUSEEVENTF_LEFTUP   = 0x0004

    class MOUSEINPUT(ctypes.Structure):
        _fields_ = [
            ("dx", ctypes.c_long), ("dy", ctypes.c_long),
            ("mouseData", ctypes.c_ulong), ("dwFlags", ctypes.c_ulong),
            ("time", ctypes.c_ulong), ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong)),
        ]

    class INPUT(ctypes.Structure):
        class _INPUT(ctypes.Union):
            _fields_ = [("mi", MOUSEINPUT)]
        _anonymous_ = ("_input",)
        _fields_ = [("type", ctypes.c_ulong), ("_input", _INPUT)]

    inputs = (INPUT * 2)()
    inputs[0].type = 0  # INPUT_MOUSE
    inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN
    inputs[1].type = 0
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP
    ctypes.windll.user32.SendInput(1, ctypes.byref(inputs[0]), ctypes.sizeof(INPUT))
    time.sleep(delay)
    ctypes.windll.user32.SendInput(1, ctypes.byref(inputs[1]), ctypes.sizeof(INPUT))


def press_key(hwnd: int, vk_code: int, delay: float = 0.05):
    """Post a key press + release directly to the specified window (WM_KEYDOWN / WM_KEYUP).

    Uses PostMessage to the target hwnd rather than global keybd_event, so the key
    always reaches the intended window regardless of which window has focus.
    This prevents ESC (and other keys) from accidentally hitting the host server window.
    """
    scan = win32api.MapVirtualKey(vk_code, 0)
    lp_down = (scan << 16) | 1           # scan code | repeat=1, key going down
    lp_up   = (scan << 16) | 0xC0000001  # scan code | repeat=1, prev=down, transition=up
    win32gui.PostMessage(hwnd, win32con.WM_KEYDOWN, vk_code, lp_down)
    time.sleep(delay)
    win32gui.PostMessage(hwnd, win32con.WM_KEYUP, vk_code, lp_up)


def press_space(hwnd: int):
    press_key(hwnd, win32con.VK_SPACE)


def press_enter(hwnd: int):
    press_key(hwnd, win32con.VK_RETURN)


def press_escape(hwnd: int):
    press_key(hwnd, win32con.VK_ESCAPE)
