"""
deploy.py — Copy freshly-built ICD DLL to all registered game directories.

Run after each build:
  python tools/deploy.py                    # deploy to all
  python tools/deploy.py sort_the_court     # deploy to one
"""

import shutil
import sys
from pathlib import Path

ICD_DLL_64 = Path(r"S:\bld\vboxgpu\build\guest_vk_icd\Debug\vbox_vulkan.dll")
ICD_DLL_32 = Path(r"S:\bld\vboxgpu\build32\guest_vk_icd\Debug\vbox_vulkan.dll")
ICD_JSON   = Path(r"S:\bld\vboxgpu\tests\dx11_triangle\test_env\vbox_icd.json")

# (dir, bits) — bits=32 → use build32 ICD, bits=64 → use build64 ICD
GAME_DIRS: dict[str, tuple[Path, int]] = {
    "dx11_triangle":  (Path(r"S:\bld\vboxgpu\tests\dx11_triangle\test_env"), 64),
    "sort_the_court": (Path(r"S:\bld\vboxgpu\tests\SortTheCourt"),            32),
    "ultrakill":      (Path(r"S:\bld\vboxgpu\tests\UltraKill"),               64),
}


def deploy_icd(targets: list[str] | None = None):
    if not ICD_JSON.exists():
        raise FileNotFoundError(f"ICD JSON not found: {ICD_JSON}")

    items = {k: v for k, v in GAME_DIRS.items()
             if targets is None or k in targets}

    for name, (d, bits) in items.items():
        icd_dll = ICD_DLL_32 if bits == 32 else ICD_DLL_64
        if not icd_dll.exists():
            print(f"  SKIP {name}: ICD DLL not found ({icd_dll})")
            continue
        if not d.exists():
            print(f"  SKIP {name}: directory not found ({d})")
            continue
        dst_dll  = d / "vbox_vulkan.dll"
        dst_json = d / "vbox_icd.json"
        shutil.copy2(icd_dll, dst_dll)
        shutil.copy2(ICD_JSON, dst_json)
        print(f"  OK   {name} ({bits}-bit): {dst_dll}")


if __name__ == "__main__":
    targets = sys.argv[1:] or None
    if targets:
        invalid = [t for t in targets if t not in GAME_DIRS.keys()]
        if invalid:
            print(f"Unknown targets: {invalid}. Valid: {list(GAME_DIRS)}")
            sys.exit(1)
    print(f"Deploying ICD DLL: {ICD_DLL}")
    deploy_icd(targets)
    print("Done.")
