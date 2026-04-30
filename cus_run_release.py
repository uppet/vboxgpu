"""
Launch Heaven 4.0 with Release ICD.
Usage: python cus_run_release.py
Host server must be running separately.
"""
import subprocess, os, sys

HEAVEN_EXE = r'S:\Apps\Heaven Benchmark 4.0\bin\Heaven.exe'
HEAVEN_CWD = r'S:\Apps\Heaven Benchmark 4.0\bin'
ICD_JSON   = r'S:\Apps\Heaven Benchmark 4.0\bin\vbox_icd.json'
ICD_SRC    = r'S:\bld\vboxgpu\build32\guest_vk_icd\Release\vbox_vulkan.dll'
ICD_DST    = r'S:\Apps\Heaven Benchmark 4.0\bin\vbox_vulkan.dll'

# Copy latest Release ICD
import shutil
shutil.copy2(ICD_SRC, ICD_DST)
print(f'Copied ICD: {ICD_SRC}')

# Clean environment: remove any inherited Vulkan vars, then set ours
env = {k: v for k, v in os.environ.items() if not k.startswith('VK_')}
env['VK_ICD_FILENAMES'] = ICD_JSON
env['VK_LOADER_LAYERS_DISABLE'] = '*'

print(f'VK_ICD_FILENAMES={ICD_JSON}')
print(f'Starting Heaven...')

proc = subprocess.Popen(
    [HEAVEN_EXE,
     '-data_path', '../',
     '-engine_config', '../data/heaven_4.0.cfg',
     '-system_script', 'heaven/unigine.cpp',
     '-sound_app', 'openal',
     '-video_app', 'direct3d11',
     '-video_mode', '-1',
     '-extern_define', 'RELEASE'],
    cwd=HEAVEN_CWD,
    env=env
)

proc.wait()
print(f'Heaven exited: {proc.returncode}')
