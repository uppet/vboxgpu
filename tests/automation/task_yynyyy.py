"""
task_yynyyy.py — 视觉感知版 SortTheCourt 游玩任务
流程：
  1. 启动 host_server + 游戏
  2. 等待主菜单（读取 host BMP 确认画面非黑）
  3. 点击 Continue 进入游戏
  4. 对五次提问分别按键盘 Y/N（游戏 UI 右上角 Y/N 提示 = 键盘输入）
  5. 每次答完按 SPACE 推进到下一位访客
  6. 按 ESC 退出

每步都截图 → 保存 PNG → 用 Read 工具让模型确认状态
"""

import time, sys, os
import win32con
sys.path.insert(0, os.path.dirname(__file__))

from capture import capture_host_frame, is_black
from input import find_hwnd, click_at, press_escape, press_key, focus_window
from process import HostServer, GuestProcess
from pathlib import Path
from PIL import Image
import numpy as np

GAME_EXE     = Path(r"S:\bld\vboxgpu\tests\SortTheCourt\SortTheCourt.exe")
TITLE_SUBSTR = "Sort the Court"
OUT_DIR      = Path(r"S:\bld\vboxgpu\automation_out")

CONTINUE_POS = (0.50, 0.32)   # Continue 按钮中心

VK_Y = 0x59   # 'Y' key
VK_N = 0x4E   # 'N' key

CHOICES = [True, True, False, True, True]   # Y Y N Y Y

def log(msg):
    print(f"[Task] {msg}", flush=True)

def snap(tag: str) -> Path:
    """截取 host 当前帧，保存为 PNG，返回路径。"""
    OUT_DIR.mkdir(exist_ok=True)
    dst = OUT_DIR / f"snap_{tag}.png"
    try:
        img = capture_host_frame(timeout=4.0)
        img.save(str(dst))
        arr = np.array(img)
        log(f"snap [{tag}] saved → {dst.name}  mean={arr.mean():.1f}")
    except Exception as e:
        log(f"snap [{tag}] FAILED: {e}")
    return dst

def run():
    OUT_DIR.mkdir(exist_ok=True)

    with HostServer() as host:
        log("host_server started")
        with GuestProcess(GAME_EXE, extra_args=[
            "-screen-width", "800", "-screen-height", "600", "-screen-fullscreen", "0"
        ]) as guest:
            log("game started, waiting for window...")
            try:
                hwnd = find_hwnd(TITLE_SUBSTR, timeout=30.0)
            except TimeoutError as e:
                log(f"ERROR: {e}"); return
            log(f"window HWND={hwnd:#x}")

            # 等菜单加载（Unity splash ~3s + 菜单稳定）
            log("waiting for menu (15s)...")
            time.sleep(15.0)

            # 截图：确认主菜单
            snap("01_menu")

            # 点击 Continue
            log("clicking Continue...")
            click_at(hwnd, *CONTINUE_POS)
            time.sleep(0.6)
            click_at(hwnd, *CONTINUE_POS)

            # 等游戏场景加载
            log("waiting for game scene (6s)...")
            time.sleep(6.0)

            # 截图：确认进入游戏
            snap("02_ingame")

            # 游玩 5 轮（每轮 = 一个访客问题）
            for i, yes in enumerate(CHOICES):
                label = "YES" if yes else "NO"
                log(f"round {i+1}/5 → pressing {label}")

                # 等问题完全出现（访客走出 + 对话框动画）
                time.sleep(3.0)

                # 截图：问题画面
                snap(f"03_round{i+1}_before_{label}")

                # 键盘按 Y 或 N
                vk = VK_Y if yes else VK_N
                focus_window(hwnd)
                press_key(hwnd, vk)

                # 等回答动画播完
                time.sleep(2.0)

                # 截图：答题后
                snap(f"04_round{i+1}_after_{label}")

                # 按 SPACE 推进到下一位访客
                press_key(hwnd, win32con.VK_SPACE)
                time.sleep(0.5)

            # ESC 退出
            log("pressing ESC to exit...")
            press_escape(hwnd)
            time.sleep(1.5)

            # 截图：退出后
            snap("05_after_esc")

            log("all done.")

if __name__ == "__main__":
    run()
