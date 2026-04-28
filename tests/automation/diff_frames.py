"""Pixel-level diff between native DXVK and bridge rendering."""
import numpy as np
from PIL import Image
from pathlib import Path

NATIVE = Path(r"S:\bld\vboxgpu\automation_out\verify_dxvk_native\frame_00.png")
BRIDGE = Path(r"S:\bld\vboxgpu\automation_out\verify_fmtfix\frame_00.png")
OUT    = Path(r"S:\bld\vboxgpu\automation_out\diff_analysis")

def analyze():
    OUT.mkdir(parents=True, exist_ok=True)

    nat = np.array(Image.open(str(NATIVE)))
    brg = np.array(Image.open(str(BRIDGE)))

    print(f"Native: {nat.shape}, Bridge: {brg.shape}")

    # Resize to same dimensions if needed
    h = min(nat.shape[0], brg.shape[0])
    w = min(nat.shape[1], brg.shape[1])
    nat = nat[:h, :w, :3]
    brg = brg[:h, :w, :3]

    # Absolute diff
    diff = np.abs(nat.astype(int) - brg.astype(int)).astype(np.uint8)
    diff_gray = diff.max(axis=2)  # max channel diff per pixel

    # Stats
    total_pixels = h * w
    thresh = 20  # pixels with diff > 20 are "corrupted"
    corrupted_mask = diff_gray > thresh
    corrupted_count = corrupted_mask.sum()
    print(f"Resolution: {w}x{h}")
    print(f"Corrupted pixels (diff>{thresh}): {corrupted_count} / {total_pixels} ({100*corrupted_count/total_pixels:.1f}%)")
    print(f"Mean diff: {diff_gray.mean():.1f}, Max diff: {diff_gray.max()}")

    # Regional analysis: split into 4x4 grid
    print("\nRegional corruption (4x4 grid, % corrupted pixels):")
    gh, gw = h // 4, w // 4
    for gy in range(4):
        row = []
        for gx in range(4):
            region = corrupted_mask[gy*gh:(gy+1)*gh, gx*gw:(gx+1)*gw]
            pct = 100 * region.sum() / region.size
            row.append(f"{pct:5.1f}%")
        print(f"  Row {gy}: {' '.join(row)}")

    # Save diff images
    # Amplified diff (x4 for visibility)
    diff_amp = np.clip(diff * 4, 0, 255).astype(np.uint8)
    Image.fromarray(diff_amp).save(str(OUT / "diff_amplified.png"))

    # Corruption mask overlay
    overlay = brg.copy()
    overlay[corrupted_mask] = [255, 0, 0]  # red for corrupted
    Image.fromarray(overlay).save(str(OUT / "corruption_overlay.png"))

    # Bridge with clean (native) pixels for corrupted areas
    fixed = brg.copy()
    fixed[corrupted_mask] = nat[corrupted_mask]
    Image.fromarray(fixed).save(str(OUT / "bridge_fixed_preview.png"))

    print(f"\nSaved to {OUT}")

if __name__ == "__main__":
    analyze()
