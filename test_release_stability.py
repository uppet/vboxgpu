"""
Release ICD stability test: run Heaven N times and report crash rate.
"""
import subprocess, os, sys, shutil, time

N = 5
TIMEOUT = 30  # seconds per run; if alive after timeout, count as OK
HEAVEN_EXE = r'S:\Apps\Heaven Benchmark 4.0\bin\Heaven.exe'
HEAVEN_CWD = r'S:\Apps\Heaven Benchmark 4.0\bin'
ICD_JSON   = r'S:\Apps\Heaven Benchmark 4.0\bin\vbox_icd.json'
ICD_SRC    = r'S:\bld\vboxgpu\build32\guest_vk_icd\Release\vbox_vulkan.dll'
ICD_DST    = r'S:\Apps\Heaven Benchmark 4.0\bin\vbox_vulkan.dll'
LOG_PATH   = r'S:\bld\vboxgpu\icd_debug.log'

# Copy latest ICD
subprocess.run(['cmd', '/c', 'copy', '/Y', ICD_SRC, ICD_DST], check=True, capture_output=True)
print(f'Copied ICD: {ICD_SRC}')

results = []
for i in range(N):
    # Clear log
    try: os.remove(LOG_PATH)
    except: pass

    env = {k: v for k, v in os.environ.items() if not k.startswith('VK_')}
    env['VK_ICD_FILENAMES'] = ICD_JSON
    env['VK_LOADER_LAYERS_DISABLE'] = '*'

    print(f'\n--- Run {i+1}/{N} ---')
    proc = subprocess.Popen(
        [HEAVEN_EXE, '-data_path', '../', '-engine_config', '../data/heaven_4.0.cfg',
         '-system_script', 'heaven/unigine.cpp', '-sound_app', 'openal',
         '-video_app', 'direct3d11', '-video_mode', '-1',
         '-extern_define', 'RELEASE'],
        cwd=HEAVEN_CWD, env=env)
    try:
        rc = proc.wait(timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        rc = 0  # survived timeout = success
        print(f'  Survived {TIMEOUT}s — OK')

    status = "CRASH" if rc != 0 else "OK"
    results.append(status)
    print(f'  Exit code: {rc} ({rc:#010x}) -> {status}')

    # Check log for CRASH entries
    try:
        with open(LOG_PATH, 'r') as f:
            log = f.read()
            if 'CRASH' in log:
                for line in log.split('\n'):
                    if 'CRASH' in line:
                        print(f'  Log: {line.strip()}')
    except: pass

    time.sleep(2)  # Brief pause between runs

print(f'\n=== RESULTS: {results.count("OK")}/{N} OK, {results.count("CRASH")}/{N} CRASH ===')
for i, r in enumerate(results):
    print(f'  Run {i+1}: {r}')
