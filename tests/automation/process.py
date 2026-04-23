"""
process.py — Host server + guest process lifecycle management.

Host server 是常驻多客户端服务：启动一次，接受多个 guest 连接。
guest 退出后 session 自动清理，host 不退出。

HostServer:
  - ensure_running(): 如果 host 没在跑就启动，已在跑则复用
  - stop(): 强制停止 host（通常不需要）
  - log_path: stderr 日志文件路径

GuestProcess:
  - start()/stop(): 管理单个 guest 进程
  - env 自动设置 VK_ICD_FILENAMES
"""

import os
import subprocess
import time
from pathlib import Path

HOST_SERVER_EXE = Path(r"S:\bld\vboxgpu\build\host\Debug\vbox_host_server.exe")
HOST_SERVER_CWD = Path(r"S:\bld\vboxgpu")

_LOG_DIR = Path(r"S:\bld\vboxgpu")
HOST_LOG = _LOG_DIR / "auto_host_err.txt"


def _host_env():
    """Host 环境：清除 VK_ICD / VK_LOADER 变量。"""
    env = dict(os.environ)
    for key in list(env):
        if key.startswith("VK_ICD") or key.startswith("VK_LOADER"):
            del env[key]
    return env


def _find_host_pid():
    """查找正在运行的 vbox_host_server.exe 的 PID。"""
    try:
        r = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq vbox_host_server.exe", "/FO", "CSV", "/NH"],
            capture_output=True, text=True, timeout=5)
        for line in r.stdout.strip().splitlines():
            parts = line.strip('"').split('","')
            if len(parts) >= 2 and "vbox_host_server" in parts[0].lower():
                return int(parts[1])
    except Exception:
        pass
    return None


class HostServer:
    """常驻 host server 管理。

    用法：
        host = HostServer()
        host.ensure_running()    # 启动或复用已有 host
        # ... 跑 guest ...
        # host 不需要停止，下次测试直接复用
        host.stop()              # 仅在需要时停止
    """

    def __init__(self):
        self._pid = None

    def ensure_running(self):
        """确保 host server 正在运行。已在运行则复用。"""
        pid = _find_host_pid()
        if pid:
            self._pid = pid
            return

        # 清空旧日志
        HOST_LOG.write_text("")

        # 启动 host：用 start 创建独立 console + stderr 重定向到文件
        subprocess.Popen(
            f'start "VBoxGPU Host" /D "{HOST_SERVER_CWD}" '
            f'cmd /c ""{HOST_SERVER_EXE}" 2>"{HOST_LOG}""',
            shell=True,
            env=_host_env(),
        )

        # 等待 host 就绪（轮询日志）
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline:
            pid = _find_host_pid()
            if pid:
                self._pid = pid
                # 检查日志确认 TCP 就绪
                try:
                    if "Listening on port" in HOST_LOG.read_text(encoding='utf-8', errors='ignore'):
                        return
                except Exception:
                    pass
            time.sleep(0.3)

        # 回退：进程在就算没看到日志也接受
        if self._pid:
            return
        raise RuntimeError("Host server failed to start within 10s")

    def stop(self):
        """停止 host server。"""
        subprocess.run(["taskkill", "/F", "/IM", "vbox_host_server.exe"],
                       capture_output=True)
        self._pid = None

    def is_alive(self):
        if not self._pid:
            return False
        return _find_host_pid() is not None

    @property
    def log_path(self):
        return HOST_LOG

    def __enter__(self):
        self.ensure_running()
        return self

    def __exit__(self, *_):
        self.stop()


class GuestProcess:
    """Manages a guest DX11 application.

    ICD env vars are derived automatically from exe_path.parent:
      VK_ICD_FILENAMES = <exe_dir>\\vbox_icd.json
    so vbox_vulkan.dll + vbox_icd.json must live alongside the exe.
    """

    def __init__(self, exe_path: Path, extra_env: dict = {}, extra_args: list = []):
        self.exe_path = exe_path
        self.extra_args = extra_args
        self.cwd = exe_path.parent
        self._stem = exe_path.stem
        self.env = {
            **os.environ,
            "VK_ICD_FILENAMES": str(exe_path.parent / "vbox_icd.json"),
            "VK_LOADER_LAYERS_DISABLE": "*",
            **extra_env,
        }
        self._proc = None
        self._stdout_f = None
        self._stderr_f = None

    def start(self):
        if self._proc is not None:
            return
        self._stdout_f = open(_LOG_DIR / f"auto_{self._stem}_out.txt", "w")
        self._stderr_f = open(_LOG_DIR / f"auto_{self._stem}_err.txt", "w")
        self._proc = subprocess.Popen(
            [str(self.exe_path)] + self.extra_args,
            cwd=str(self.cwd),
            env=self.env,
            stdout=self._stdout_f,
            stderr=self._stderr_f,
        )

    def stop(self, timeout: float = 5.0):
        if self._proc is None:
            return
        self._proc.terminate()
        try:
            self._proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self._proc.kill()
        self._proc = None
        for f in (self._stdout_f, self._stderr_f):
            if f:
                try: f.close()
                except Exception: pass
        self._stdout_f = self._stderr_f = None

    def is_alive(self):
        return self._proc is not None and self._proc.poll() is None

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *_):
        self.stop()
