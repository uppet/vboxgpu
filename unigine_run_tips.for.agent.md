# Unigine Heaven 4.0 — Agent 运行备忘

## 基本信息
- 安装路径: `S:\Apps\Heaven Benchmark 4.0\`
- 可执行: `bin\Heaven.exe` (PE32, 32-bit)
- 窗口标题: `Unigine Heaven Benchmark 4.0 Basic (Direct3D11)`
- capture_helper 用 `Unigine` 作为 title substr

## 启动方式

### ❌ 不要用 delegate-runner 启动 host server
delegate-runner 的 `start` 命令 + Python subprocess 组合经常失败：
- "The current directory is invalid"
- 路径转义问题（空格、反斜杠）
- host 进程未正确启动但无明确错误

### ✅ 正确做法：手动启动 host，或确认 host 已在运行
1. 用户手动启动 host: 双击运行或通过 cmd
2. 或用 automation 框架的 `HostServer()` context manager（它用 `start` + 正确的环境清理）
3. 然后用 delegate-runner 只启动 guest (Heaven.exe)

### ✅ 启动 Heaven 的正确方式
```python
import subprocess, os
env = {**os.environ,
    'VK_ICD_FILENAMES': r'S:\Apps\Heaven Benchmark 4.0\bin\vbox_icd.json',
    'VK_LOADER_LAYERS_DISABLE': '*'}
proc = subprocess.Popen(
    [r'S:\Apps\Heaven Benchmark 4.0\bin\Heaven.exe',
     '-data_path', '../', '-engine_config', '../data/heaven_4.0.cfg',
     '-system_script', 'heaven/unigine.cpp', '-sound_app', 'openal',
     '-video_app', 'direct3d11', '-video_mode', '-1',
     '-extern_define', 'RELEASE'],
    cwd=r'S:\Apps\Heaven Benchmark 4.0\bin', env=env)
```

## 必要文件 (bin/ 目录下)
- `unigine.cfg` — 引擎运行时配置（无此文件会 "can't initialize filesystem" 退出）
- `d3d11.dll` / `dxgi.dll` — DXVK 32-bit DLL
- `vbox_vulkan.dll` — 32-bit ICD (来自 build32)
- `vbox_icd.json` — ICD 配置

## 更新 ICD
```bash
cp S:/bld/vboxgpu/build32/guest_vk_icd/Debug/vbox_vulkan.dll "S:/Apps/Heaven Benchmark 4.0/bin/vbox_vulkan.dll"
```
注意：Heaven.exe 运行时会锁定 DLL，必须先杀进程再复制。

## 已知行为
- 渲染约 25 帧后 Heaven 调用 `video_restart()` 重建 D3D11 device，第二次 CreateDevice 会失败退出
- host EndRendering 可能触发 NVIDIA driver null deref（已有 SEH 保护）
- Debug 构建 FPS ~7
- 32-bit 进程地址空间有限，DEVICE_LOCAL 内存不预分配 shadow

## 偶发卡死/退出处理
Heaven 运行时偶尔会卡死或提前退出，但下一次运行就正常。这是已知的不稳定性，不是新 bug。
**规则：偶发的卡死或退出，先重试 1-2 次再判断。不要立即归因为代码问题去改代码，避免打断渲染问题的排查节奏。**

## Overlap 分析流程
1. 运行 Heaven，收集 host_err.txt
2. `grep -a "BindImageMemory" host_err.txt > heaven_binds.txt`
3. 用 Python 解析 offset/reqSize，按 memId 分组，排序后检查相邻 image 是否重叠
4. 找到重叠的 image ID 后 `grep "CreateImage: id=XXX"` 查格式和尺寸
5. 检查 formatBpp 是否覆盖该格式

## Release 异步协议问题（重要）
Release ICD 的异步协议 (AcquireNextImage 非阻塞 + rotating index) 会导致 DXVK C++ 异常崩溃 (0xE06D7363)，表现为 Heaven 提前退出。
- Debug ICD 异步协议正常工作
- Release ICD 回退到阻塞协议后，不仅稳定运行，**金属材质也渲染正常**（亮绿色消失）
- 说明异步协议的 rotating index（不等 host response 的 imageIndex）在 Release 下导致 swapchain image 不同步 → 渲染错乱 → DXVK 抛异常
- 当前方案：`#ifdef _DEBUG` 异步协议只在 Debug 生效，Release 用阻塞协议
- TODO: 修复异步协议让 Release 也能用（可能需要保留阻塞等 imageIndex 但去掉等帧数据的部分）

## 编译注意
**每次改代码必须同时编译 Debug + Release（host + ICD 32/64）。** 用户可能用 Release host 测试，只编 Debug 会导致新功能不生效。
```bash
cmake --build S:/bld/vboxgpu/build --config Debug --target vbox_host_server --target vbox_vk_icd
cmake --build S:/bld/vboxgpu/build --config Release --target vbox_host_server --target vbox_vk_icd
cmake --build S:/bld/vboxgpu/build32 --config Debug --target vbox_vk_icd
cmake --build S:/bld/vboxgpu/build32 --config Release --target vbox_vk_icd
```

## 截图验证
```bash
capture_helper.exe Unigine S:/bld/vboxgpu/automation_out/heaven_scene.png
```
