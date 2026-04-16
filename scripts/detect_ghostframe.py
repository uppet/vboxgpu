#!/usr/bin/env python3
"""
detect_ghostframe.py — 花屏（anomalous frame）检测工具

用法：
  # 先用 replay 保存全帧：
  vbox_host_server.exe --replay dump.bin --save-frames S:/bld/vboxgpu/frames/sc_xxx

  # 然后运行检测：
  python3 detect_ghostframe.py --frames-dir /mnt/s/bld/vboxgpu/frames/sc_xxx
  python3 detect_ghostframe.py --frames-dir DIR [--window 5] [--no-png]

检测逻辑：
  1. 所有帧（BMP）都转为 PNG 保存
  2. 计算相邻帧之差（diff score）和空间无序度（disorder score）
  3. 在 5 帧滑动窗口中：
     - 场景转场：diff 高但连续（>=3 帧），不报告
     - 花屏：单帧 diff 突然飙高（是邻居均值的 N 倍）且 disorder 高（散乱像素）
  4. 输出：flagged frames + 报告
"""

import os
import sys
import struct
import zlib
import math
import argparse
import glob
import statistics

# ---------------------------------------------------------------------------
# BMP reader — handles 24bpp and 32bpp, top-down and bottom-up
# ---------------------------------------------------------------------------

def read_bmp(path):
    """Returns (width, height, rgb_bytes) where rgb_bytes is row-major RGB."""
    with open(path, 'rb') as f:
        data = f.read()

    if data[:2] != b'BM':
        raise ValueError(f'Not a BMP: {path}')

    px_off   = struct.unpack_from('<I', data, 10)[0]
    w        = struct.unpack_from('<i', data, 18)[0]
    h_raw    = struct.unpack_from('<i', data, 22)[0]
    bpp      = struct.unpack_from('<H', data, 28)[0]
    h        = abs(h_raw)
    flip     = h_raw > 0   # positive h = bottom-up storage

    stride = bpp // 8
    row_stride = (w * stride + 3) & ~3   # BMP rows are DWORD-aligned
    px = data[px_off:]

    rgb = bytearray(w * h * 3)
    for row in range(h):
        src_row = (h - 1 - row) if flip else row
        row_off = src_row * row_stride
        for col in range(w):
            p = row_off + col * stride
            b = px[p]; g = px[p+1]; r = px[p+2]
            dst = (row * w + col) * 3
            rgb[dst] = r; rgb[dst+1] = g; rgb[dst+2] = b

    return w, h, bytes(rgb)


# ---------------------------------------------------------------------------
# Minimal PNG writer (no external deps)
# ---------------------------------------------------------------------------

def write_png(path, width, height, rgb_bytes):
    """Write a minimal RGB PNG."""
    def _chunk(tag, payload):
        data = tag + payload
        return struct.pack('>I', len(payload)) + data + struct.pack('>I', zlib.crc32(data) & 0xffffffff)

    ihdr = struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)

    raw = bytearray()
    for row in range(height):
        raw += b'\x00'  # filter type 0 (None)
        raw += rgb_bytes[row * width * 3 : (row + 1) * width * 3]

    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(_chunk(b'IHDR', ihdr))
        f.write(_chunk(b'IDAT', zlib.compress(bytes(raw), 6)))
        f.write(_chunk(b'IEND', b''))


# ---------------------------------------------------------------------------
# Per-pixel diff metrics between two frames (RGB bytes)
# ---------------------------------------------------------------------------

def compute_diff(rgb_a, rgb_b, w, h):
    """
    Returns:
      mean_diff    — average absolute channel difference per pixel (0..255)
      disorder     — spatial disorder coefficient (higher = more scattered changes)
      diff_pixels  — number of pixels with mean channel diff > CHANGED_THRESH
      max_diff     — maximum per-pixel mean diff
    """
    CHANGED_THRESH = 15   # pixel considered "changed" if mean channel diff > this

    n = w * h
    total_diff = 0
    diff_vals = []
    changed_positions = []   # (row, col) of changed pixels

    for i in range(n):
        p = i * 3
        dr = abs(rgb_a[p]   - rgb_b[p])
        dg = abs(rgb_a[p+1] - rgb_b[p+1])
        db = abs(rgb_a[p+2] - rgb_b[p+2])
        d = (dr + dg + db) // 3
        total_diff += d
        diff_vals.append(d)
        if d > CHANGED_THRESH:
            changed_positions.append(i)

    mean_diff = total_diff / n
    max_diff  = max(diff_vals) if diff_vals else 0
    diff_pixels = len(changed_positions)

    # Spatial disorder: fraction of changed pixels that have no changed neighbours.
    # High isolation ratio → scattered (花屏-like).  Low → clustered (normal motion/transition).
    disorder = 0.0
    if diff_pixels > 10:
        changed_set = set(changed_positions)
        isolated = 0
        for idx in changed_positions:
            row, col = divmod(idx, w)
            # Check 4 neighbours
            has_neighbour = (
                (row > 0   and (idx - w) in changed_set) or
                (row < h-1 and (idx + w) in changed_set) or
                (col > 0   and (idx - 1) in changed_set) or
                (col < w-1 and (idx + 1) in changed_set)
            )
            if not has_neighbour:
                isolated += 1
        disorder = isolated / diff_pixels

    return mean_diff, disorder, diff_pixels, max_diff


# ---------------------------------------------------------------------------
# 花屏 detection in sliding window
# ---------------------------------------------------------------------------

def detect_ghostframes(frame_metrics, window=5,
                       spike_ratio=3.0,     # bi_diff must be > spike_ratio × neighbor bi_diff median
                       min_mean_diff=8.0):  # absolute floor for bi_diff
    """
    花屏检测：使用双向 diff 突峰（bilateral spike）算法。

    关键洞察：花屏帧的特征是"对两侧邻帧都差异巨大"，而非"散乱像素"。
    定义 bi_diff[i] = min(diff[i], diff[i+1])，即帧 i 对前后两帧的
    较小差异值。花屏帧的 bi_diff 会突然飙高，而其两侧的 bi_diff 很低。
    场景转场会导致连续多帧 bi_diff 高 → 被 transition check 过滤。

    frame_metrics: list of (mean_diff, disorder, diff_pixels, max_diff)
                   其中 diff[i] = diff(frame_{i-1}, frame_i)
    Returns list of (frame_index, reason_str)
    """
    n = len(frame_metrics)
    if n < 3:
        return []

    # Build bi-directional diff: bi_diff[i] = min(diff[i], diff[i+1])
    # High bi_diff[i] means frame i is far from BOTH frame_{i-1} AND frame_{i+1}.
    bi = []
    for i in range(n):
        d_in  = frame_metrics[i][0]
        d_out = frame_metrics[i + 1][0] if i + 1 < n else d_in
        bi.append(min(d_in, d_out))

    half = window // 2
    results = []

    for i in range(half, n - half - 1):  # -1: need bi[i+1] defined
        bi_i = bi[i]

        # Neighbor bi_diffs (exclude i itself)
        neighbour_bi = [bi[j] for j in range(i - half, i + half + 1) if j != i]
        neighbour_median = statistics.median(neighbour_bi) if neighbour_bi else 0

        # Scene transition: 3+ frames with high bi_diff in window → skip
        high_count = sum(1 for j in range(i - half, i + half + 1)
                         if bi[j] > min_mean_diff * 2)
        is_transition = high_count >= 3

        # Bilateral spike: frame i's bi_diff >> neighbours
        is_spike = (bi_i > min_mean_diff and
                    (neighbour_median < 1.0 or bi_i > spike_ratio * neighbour_median))

        if is_spike and not is_transition:
            d_in  = frame_metrics[i][0]
            d_out = frame_metrics[i + 1][0] if i + 1 < n else 0
            reason = (f"bi_diff={bi_i:.1f} (in={d_in:.1f}, out={d_out:.1f}), "
                      f"neighbor_bi_med={neighbour_median:.1f}, "
                      f"ratio={bi_i/(neighbour_median+0.001):.1f}x")
            results.append((i, reason))

    return results


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--frames-dir', required=True,
                    help='Directory containing frame_NNNN.bmp files from --save-frames')
    ap.add_argument('--window', type=int, default=5,
                    help='Sliding window size (default: 5)')
    ap.add_argument('--spike-ratio', type=float, default=3.0,
                    help='Bilateral spike ratio vs neighbour bi_diff median (default: 3.0)')
    ap.add_argument('--no-png', action='store_true',
                    help='Skip PNG conversion (only output report)')
    ap.add_argument('--only-flagged-png', action='store_true',
                    help='Only save PNG for flagged frames (faster)')
    args = ap.parse_args()

    frames_dir = args.frames_dir.rstrip('/\\')

    # Collect BMP files
    bmp_files = sorted(glob.glob(os.path.join(frames_dir, 'frame_*.bmp')))
    if not bmp_files:
        print(f'No frame_NNNN.bmp files found in {frames_dir}')
        sys.exit(1)

    print(f'Found {len(bmp_files)} frames in {frames_dir}')

    # Load frames and compute metrics
    frames = []          # list of (w, h, rgb_bytes)
    frame_metrics = []   # list of (mean_diff, disorder, diff_pixels, max_diff)

    prev_rgb = None
    for idx, bmp_path in enumerate(bmp_files):
        print(f'\rLoading frame {idx+1}/{len(bmp_files)}...', end='', flush=True)
        try:
            w, h, rgb = read_bmp(bmp_path)
        except Exception as e:
            print(f'\n  ERROR reading {bmp_path}: {e}')
            continue

        if prev_rgb is not None and len(prev_rgb) == len(rgb):
            metrics = compute_diff(prev_rgb, rgb, w, h)
        else:
            metrics = (0.0, 0.0, 0, 0)  # first frame has no diff

        frames.append((w, h, rgb, bmp_path))
        frame_metrics.append(metrics)
        prev_rgb = rgb

    print(f'\rLoaded {len(frames)} frames.         ')

    if not frames:
        print('No valid frames loaded.')
        sys.exit(1)

    # Detect 花屏
    flagged = detect_ghostframes(frame_metrics,
                                 window=args.window,
                                 spike_ratio=args.spike_ratio)

    flagged_set = set(i for i, _ in flagged)

    # Save PNGs
    if not args.no_png:
        png_dir = frames_dir  # save alongside BMPs
        total_to_save = len(frames) if not args.only_flagged_png else len(flagged_set) * (args.window + 2)
        print(f'Saving PNGs to {png_dir}...')
        saved = 0
        for idx, (w, h, rgb, src_path) in enumerate(frames):
            if args.only_flagged_png:
                # Save window around each flagged frame
                near_flagged = any(abs(idx - fi) <= args.window for fi, _ in flagged)
                if not near_flagged:
                    continue

            fname = os.path.basename(src_path).replace('.bmp', '.png')
            png_path = os.path.join(png_dir, fname)
            if idx in flagged_set:
                # Also save a highlighted copy with "_GHOSTFRAME" suffix
                ghost_path = os.path.join(png_dir, fname.replace('.png', '_GHOSTFRAME.png'))
                # Draw a red border (8px) on the highlighted copy
                bordered = bytearray(rgb)
                for row in range(h):
                    for col in range(w):
                        if row < 8 or row >= h-8 or col < 8 or col >= w-8:
                            p = (row * w + col) * 3
                            bordered[p] = 255; bordered[p+1] = 0; bordered[p+2] = 0
                write_png(ghost_path, w, h, bytes(bordered))

            write_png(png_path, w, h, rgb)
            saved += 1
            print(f'\r  Saved {saved} PNGs...', end='', flush=True)

        print(f'\r  Saved {saved} PNGs.          ')

    # Print report
    print()
    print('='*70)
    print(f'REPORT: {len(frames)} frames analyzed')
    print(f'Sliding window: {args.window}, spike_ratio: {args.spike_ratio}')
    print('='*70)

    print(f'\nFrame diff statistics (mean_diff per frame):')
    diffs = [m[0] for m in frame_metrics]
    if diffs:
        print(f'  min={min(diffs):.2f}  max={max(diffs):.2f}  '
              f'median={statistics.median(diffs):.2f}  '
              f'mean={statistics.mean(diffs):.2f}')

    print(f'\nTop 10 highest-diff frames:')
    top10 = sorted(enumerate(frame_metrics), key=lambda x: x[1][0], reverse=True)[:10]
    for idx, (md, dis, dp, mxd) in top10:
        flag = ' <<< GHOSTFRAME' if idx in flagged_set else ''
        print(f'  frame_{idx:04d}: diff={md:.1f} disorder={dis:.2f} '
              f'changed_px={dp} max={mxd}{flag}')

    print(f'\n花屏 frames detected: {len(flagged)}')
    for frame_idx, reason in flagged:
        fname = os.path.basename(bmp_files[frame_idx]) if frame_idx < len(bmp_files) else f'frame_{frame_idx:04d}'
        print(f'  [GHOSTFRAME] {fname}: {reason}')
        print(f'    Neighbors: ', end='')
        half = args.window // 2
        for j in range(max(0, frame_idx-half), min(len(frame_metrics), frame_idx+half+1)):
            marker = '>>>' if j == frame_idx else '   '
            print(f'{marker}f{j:04d}(diff={frame_metrics[j][0]:.1f},dis={frame_metrics[j][1]:.2f}) ', end='')
        print()

    if not flagged:
        print('  (none detected — rendering looks clean)')

    print()
    if not args.no_png and flagged:
        print(f'Flagged frame PNGs saved with _GHOSTFRAME suffix in {frames_dir}')

    return 0 if not flagged else 1


if __name__ == '__main__':
    sys.exit(main())
