"""
capture.py — WGC screenshot wrapper + pHash image comparison.

Calls capture_helper.exe (C++/WinRT) for actual capture.
All functions that return images return PIL.Image objects.
"""

import subprocess
import tempfile
import time
from pathlib import Path

import numpy as np
from PIL import Image

CAPTURE_TOOL = Path(r"S:\bld\vboxgpu\build\tools\capture_helper\Debug\capture_helper.exe")

# ---------------------------------------------------------------------------
# Capture
# ---------------------------------------------------------------------------

def capture_window(title_substr: str, out_path: Path = None) -> Image.Image:
    """Capture the first visible window whose title contains title_substr.

    If out_path is None, uses a temp file.
    Returns a PIL Image. Raises RuntimeError on failure.
    """
    if not CAPTURE_TOOL.exists():
        raise RuntimeError(f"capture_helper not found: {CAPTURE_TOOL}")

    tmp = None
    if out_path is None:
        tmp = tempfile.NamedTemporaryFile(suffix=".png", delete=False)
        tmp.close()
        out_path = Path(tmp.name)

    try:
        result = subprocess.run(
            [str(CAPTURE_TOOL), title_substr, str(out_path)],
            capture_output=True, text=True, timeout=10
        )
        if result.returncode != 0:
            raise RuntimeError(f"capture_helper failed: {result.stderr.strip()}")
        return Image.open(out_path).copy()
    finally:
        if tmp is not None:
            out_path.unlink(missing_ok=True)


# ---------------------------------------------------------------------------
# Basic image checks
# ---------------------------------------------------------------------------

def is_black(img: Image.Image, threshold: int = 8) -> bool:
    """True if the image is essentially black (mean brightness < threshold)."""
    arr = np.array(img.convert("L"), dtype=float)
    return float(arr.mean()) < threshold


# ---------------------------------------------------------------------------
# pHash — perceptual hash via 2D DCT (numpy only, no scipy)
# ---------------------------------------------------------------------------

def _dct1d_rows(a: np.ndarray) -> np.ndarray:
    """Apply 1D DCT-II along the last axis of a 2D array using numpy FFT."""
    n = a.shape[-1]
    # Mirror trick: DCT via real FFT of [x, reverse(x)]
    v = np.concatenate([a, a[:, ::-1]], axis=-1)
    F = np.fft.rfft(v, axis=-1)[..., :n]
    k = np.arange(n)
    W = np.exp(-1j * np.pi * k / (2.0 * n))
    return np.real(F * W)


def _dct2d(block: np.ndarray) -> np.ndarray:
    """2D DCT-II: apply 1D DCT along rows, then columns."""
    tmp = _dct1d_rows(block)
    return _dct1d_rows(tmp.T).T


def phash(img: Image.Image, hash_size: int = 8) -> int:
    """Compute perceptual hash (pHash) of an image.

    Returns an integer with hash_size**2 bits.
    hash_size=8 → 64-bit hash.
    """
    n = hash_size * 4  # work at 4x then take low-freq block
    small = img.convert("L").resize((n, n), Image.LANCZOS)
    pix = np.array(small, dtype=float)

    dct = _dct2d(pix)
    low = dct[:hash_size, :hash_size]

    # Threshold at median → binary hash
    median = np.median(low)
    bits = (low > median).flatten()

    result = 0
    for b in bits:
        result = (result << 1) | int(b)
    return result


def phash_hex(img: Image.Image, hash_size: int = 8) -> str:
    """pHash as hex string (for JSON storage)."""
    h = phash(img, hash_size)
    digits = hash_size * hash_size // 4
    return format(h, f"0{digits}x")


def phash_diff(a: int, b: int) -> int:
    """Hamming distance between two pHash values (0=identical, 64=totally different)."""
    return bin(a ^ b).count("1")


def frames_differ(img_a: Image.Image, img_b: Image.Image,
                  hamming_threshold: int = 10) -> bool:
    """True if two frames differ significantly (pHash hamming distance >= threshold)."""
    return phash_diff(phash(img_a), phash(img_b)) >= hamming_threshold


# ---------------------------------------------------------------------------
# Polling helpers
# ---------------------------------------------------------------------------

def wait_for_nonblack(title_substr: str,
                      timeout: float = 30.0,
                      poll_interval: float = 0.5) -> Image.Image:
    """Poll window capture until a non-black frame appears.

    Returns the first non-black Image. Raises TimeoutError if timeout exceeded.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            img = capture_window(title_substr)
            if not is_black(img):
                return img
        except Exception:
            pass
        time.sleep(poll_interval)
    raise TimeoutError(f"wait_for_nonblack: '{title_substr}' still black after {timeout}s")


def capture_host_frame(timeout: float = 3.0) -> Image.Image:
    """Request a screenshot from the host server via flag-file signaling.

    Creates dbg_capture_request, waits for host to write dbg_latest.bmp,
    returns it as a PIL Image. Raises RuntimeError on timeout.
    """
    req = Path(r"S:\bld\vboxgpu\dbg_capture_request")
    out = Path(r"S:\bld\vboxgpu\dbg_latest.bmp")

    out.unlink(missing_ok=True)   # remove stale
    req.touch()                   # signal host

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        # Host deletes req and writes out atomically
        if not req.exists() and out.exists():
            try:
                img = Image.open(str(out)).copy()
                return img
            except Exception:
                pass
        time.sleep(0.05)

    req.unlink(missing_ok=True)
    raise RuntimeError(f"capture_host_frame: no response from host within {timeout}s")


def wait_for_stable_frame(title_substr: str,
                          stable_duration: float = 1.5,
                          poll_interval: float = 0.4,
                          timeout: float = 30.0) -> Image.Image:
    """Poll until the window shows a stable (non-changing) frame.

    'Stable' means consecutive captures are pHash-similar for at least stable_duration seconds.
    Returns the stable frame. Raises TimeoutError on timeout.
    """
    deadline = time.monotonic() + timeout
    prev = None
    stable_since = None

    while time.monotonic() < deadline:
        try:
            curr = capture_window(title_substr)
        except Exception:
            time.sleep(poll_interval)
            continue

        if prev is not None and not frames_differ(prev, curr):
            if stable_since is None:
                stable_since = time.monotonic()
            elif time.monotonic() - stable_since >= stable_duration:
                return curr
        else:
            stable_since = None

        prev = curr
        time.sleep(poll_interval)

    raise TimeoutError(f"wait_for_stable_frame: '{title_substr}' not stable after {timeout}s")
