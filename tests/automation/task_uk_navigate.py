"""
task_uk_navigate.py — Vision-guided UltraKill 导航任务 v3
流程：
  1. 启动 host_server + ULTRAKILL.exe
  2. 等待初始加载（shader 编译，需 30s+）
  3. ESC × 2 跳过 logo / violence notice（如果还在显示）
  4. 等主菜单出现（host 开始 present 非黑帧）
  5. 点击 CONTINUE（x=0.50, y=0.44）
  6. 难度屏：STANDARD 已选中（存档），直接 ENTER 确认
  7. 章节屏：点击 PRELUDE + ENTER
  8. 关卡屏：点击 DOUBLE DOWN（中间）
  → 3D 场景渲染

v3 变更：
- press_key 改用 PostMessage(WM_KEYDOWN) → ESC 不再发到 host 窗口
- 难度屏改用 ENTER 确认（不再点击已选项）
- 每步 host.is_alive() 检查
- 更多快照节点 + 更保守的等待时间
"""

import time, sys, os, win32con
sys.path.insert(0, os.path.dirname(__file__))

from capture import capture_host_frame
from input import find_hwnd, click_at, press_escape, press_key, focus_window
from process import HostServer, GuestProcess
from pathlib import Path
import numpy as np

GAME_EXE     = Path(r"S:\bld\vboxgpu\tests\UltraKill\ULTRAKILL.exe")
TITLE_SUBSTR = "ULTRAKILL"
OUT_DIR      = Path(r"S:\bld\vboxgpu\automation_out\uk")

def log(msg):
    print(f"[UK] {msg}", flush=True)

def snap(tag: str, host=None, retries: int = 3, timeout: float = 5.0):
    """截图并保存，失败重试。若提供 host 对象，先检查 host 是否存活。"""
    if host is not None and not host.is_alive():
        log(f"snap [{tag}] SKIP — host is dead")
        return None, 0.0
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    dst = OUT_DIR / f"snap_{tag}.png"
    for attempt in range(retries):
        try:
            img = capture_host_frame(timeout=timeout)
            img.save(str(dst))
            arr = np.array(img)
            mean = arr.mean()
            log(f"snap [{tag}] mean={mean:.1f}  → {dst.name}")
            return dst, mean
        except Exception as e:
            if attempt < retries - 1:
                log(f"snap [{tag}] attempt {attempt+1} failed: {e}, retrying...")
                time.sleep(2.0)
            else:
                log(f"snap [{tag}] FAILED after {retries} attempts: {e}")
    return dst, 0.0

def wait_for_frame(label: str, host, min_mean: float = 2.0, poll: float = 2.0, max_wait: float = 30.0):
    """等待 host 开始 present 非黑帧。返回 mean 值。"""
    deadline = time.monotonic() + max_wait
    while time.monotonic() < deadline:
        if not host.is_alive():
            log(f"[{label}] host died, aborting wait")
            return 0.0
        try:
            img = capture_host_frame(timeout=4.0)
            arr = np.array(img)
            mean = arr.mean()
            if mean >= min_mean:
                log(f"[{label}] frame ready: mean={mean:.1f}")
                return mean
            log(f"[{label}] frame black (mean={mean:.1f}), waiting...")
        except Exception as e:
            log(f"[{label}] no frame yet: {e}")
        time.sleep(poll)
    log(f"[{label}] timeout waiting for frame")
    return 0.0

def run():
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    with HostServer() as host:
        log("host_server started")
        with GuestProcess(GAME_EXE, extra_args=[
            "-screen-width", "800", "-screen-height", "600", "-screen-fullscreen", "0"
        ]) as guest:
            log("game started, waiting for window...")
            try:
                hwnd = find_hwnd(TITLE_SUBSTR, timeout=40.0)
            except TimeoutError as e:
                log(f"ERROR: {e}"); return
            log(f"window HWND={hwnd:#x}")

            # UltraKill IL2CPP shader 编译很慢，等 35s
            # 经验：30s 后 logo 已自动消失，main menu 已出现
            log("waiting for initial load (35s)...")
            time.sleep(35.0)
            snap("00_after_35s", host)

            if not host.is_alive():
                log("ERROR: host died during load"); return

            # ESC × 2 跳过任何残留的 logo / violence notice
            # 若已在主菜单，ESC 无效果（安全）
            # 注意：press_key 现在用 PostMessage(WM_KEYDOWN)，
            #       只发到游戏窗口，不会误发给 host
            log("ESC × 2: skip logo / violence notice (if any)...")
            press_escape(hwnd)
            time.sleep(3.0)
            snap("01_after_esc1", host)

            press_escape(hwnd)
            time.sleep(3.0)
            snap("02_after_esc2", host)

            # 等主菜单出现（host 开始渲染非黑帧）
            log("waiting for main menu (non-black frame)...")
            mean = wait_for_frame("main_menu", host, min_mean=2.0, max_wait=20.0)
            time.sleep(1.0)
            snap("03_main_menu", host)

            if not host.is_alive():
                log("ERROR: host died before CONTINUE"); return

            # 点击 CONTINUE
            # 实测 (0.50, 0.50) 有效，(0.50, 0.44) 失效
            # Unity 按钮 hitbox 比可见文本区域更大
            log("clicking CONTINUE (0.50, 0.50)...")
            click_at(hwnd, 0.50, 0.50)
            time.sleep(4.0)
            snap("04_after_continue", host)

            if not host.is_alive():
                log("ERROR: host died after CONTINUE"); return

            # 难度选择屏：STANDARD 已选中（存档 * 标记）
            # 策略：
            #   1. hover 到 STANDARD，等 hover 动画触发
            #   2. 左键单击
            #   3. 再次单击（某些 ULTRAKILL UI 需要两次点击确认）
            #   4. 点击右侧 checkbox (x≈0.295) 作为备用
            #   5. SPACE（游戏确认键）
            log("selecting STANDARD difficulty...")
            # hover 等待
            from input import move_to
            move_to(hwnd, 0.163, 0.465)
            time.sleep(0.4)
            # 第一次点击（文字区域）
            click_at(hwnd, 0.163, 0.465)
            time.sleep(0.4)
            snap("04b_after_std_click1", host)
            # 第二次点击
            click_at(hwnd, 0.163, 0.465)
            time.sleep(0.3)
            # 点击右侧 checkbox
            click_at(hwnd, 0.295, 0.465)
            time.sleep(0.3)
            # SPACE 确认
            press_key(hwnd, win32con.VK_SPACE)
            time.sleep(3.5)
            snap("05_after_difficulty_confirm", host)

            if not host.is_alive():
                log("ERROR: host died after difficulty confirm"); return

            # 章节选择：PRELUDE（坐标待视觉验证）
            log("clicking PRELUDE area (0.163, 0.35)...")
            move_to(hwnd, 0.163, 0.35)
            time.sleep(0.3)
            click_at(hwnd, 0.163, 0.35)
            time.sleep(0.3)
            click_at(hwnd, 0.163, 0.35)
            time.sleep(0.3)
            press_key(hwnd, win32con.VK_SPACE)
            time.sleep(3.5)
            snap("06_after_prelude", host)

            if not host.is_alive():
                log("ERROR: host died after prelude"); return

            snap("06b_level_screen", host)

            # 关卡选择：DOUBLE DOWN（中间）
            log("clicking DOUBLE DOWN area (0.50, 0.50)...")
            move_to(hwnd, 0.50, 0.50)
            time.sleep(0.3)
            click_at(hwnd, 0.50, 0.50)
            time.sleep(0.3)
            click_at(hwnd, 0.50, 0.50)
            time.sleep(5.0)
            snap("07_loading", host)

            # 等 3D 场景加载
            log("waiting for 3D scene (20s)...")
            time.sleep(20.0)
            snap("08_ingame", host)

            log("done. keeping alive 5s for observation...")
            time.sleep(5.0)

if __name__ == "__main__":
    run()
