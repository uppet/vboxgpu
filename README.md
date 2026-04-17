# VBox GPU Bridge

为 VirtualBox Windows 7 虚拟机提供 DX11 级别 3D 加速。

## 工作原理

```
Guest (VM)                               Host (物理机)
┌──────────────────────┐                ┌──────────────────────┐
│ DX11 App (32/64-bit) │                │                      │
│   ↓                  │                │  Venus Decoder       │
│ DXVK 2.7.1           │    TCP loopback│   ↓                  │
│  (DX11 → Vulkan)     │ ─────────────→ │  Host Vulkan GPU     │
│   ↓                  │  Venus 命令流  │   ↓                  │
│ ICD Proxy            │ ←───────────── │  LZ4 压缩帧回传      │
│ (vbox_vulkan.dll)    │  压缩帧数据    │  Host 窗口显示       │
└──────────────────────┘                └──────────────────────┘
```

Guest 侧的 DXVK 将 DX11 调用翻译为 Vulkan，经我们的 ICD 代理拦截后序列化为 Venus 命令流，通过 TCP 传输到 Host，Host 解码后调用真实 Vulkan GPU 渲染，渲染结果 LZ4 压缩后回传 Guest 显示。

## 当前状态 (M1.6 — 2026-04)

**阶段一已基本完成** — 真实 Unity 游戏 (SortTheCourt) 通过完整链路以 **60 FPS** 可玩运行。

### 已验证游戏
| 游戏 | 引擎 | 架构 | 状态 |
|------|------|------|------|
| SortTheCourt | Unity 5.3 | 32-bit | ✓ 60 FPS 可玩 |
| UltraKill (demo) | Unity | 64-bit | ⚠️ Loading ~60 FPS，游玩 ~20 FPS，有零星渲染错误 |

### 已完成功能

**渲染核心**
- [x] Vulkan ICD 代理框架（Vulkan 1.3, 60+ 扩展，DXVK 2.7.1 兼容）
- [x] Venus 命令流编解码（完整 Vulkan 命令子集）
- [x] Dynamic Rendering（VK_KHR_dynamic_rendering）
- [x] 深度测试（5 个深度动态状态 + depth attachment）
- [x] Alpha blend（全 blend state 序列化）
- [x] BDA (BufferDeviceAddress) 同步转发（DXVK 2.7.1 必需）
- [x] 大型 mapped memory 传输（16MB+ descriptor heap 全量 flush）

**GPU 资源序列化**
- [x] Image / Memory / ImageView / Sampler
- [x] Buffer（含 SHADER_DEVICE_ADDRESS）
- [x] DescriptorSet / DescriptorLayout / PipelineLayout
- [x] Swapchain / Framebuffer / RenderPass
- [x] 资源销毁全链路（15 个 vkDestroy*/vkFreeMemory）

**传输层**
- [x] TCP loopback（Host server 独立线程，窗口始终响应）
- [x] 命令流录制/回放（--dump / --replay 模式）
- [x] 画面回传：LZ4 压缩 + 异步 recv + GDI StretchDIBits（~60 FPS）
- [x] 双缓冲 readback（GPU copy 与 CPU 发送流水线化）

**兼容性**
- [x] 32-bit ICD（build32 目录，支持 PE32 游戏）
- [x] Venus codegen 框架（59/67 API 可自动生成，按需集成）

### 待完成（M1.6 剩余 / M2 规划）
- [ ] Stencil test
- [ ] Mipmap 生成/采样
- [ ] 多 Render Target (MRT)
- [ ] Mapped memory 增量传输（dirty tracking 优化，当前全量）
- [ ] 更多像素格式（BC 压缩纹理、R16、R32F）
- [ ] 阶段二：WDDM UMD 驱动下沉（脱离 DXVK 魔改）

## 项目结构

```
common/
  venus/              命令编解码 (vn_command.h, vn_encoder.h, vn_stream.h)
  vboxgpu_config.h    全局配置（dirty tracking 阈值等）

guest_vk_icd/         Vulkan ICD 代理 → vbox_vulkan.dll (32/64-bit)
  src/icd_dispatch.cpp   Vulkan 入口 + Venus 编码 + flush 策略

host/
  src/
    main_server.cpp      TCP server 模式入口
    vk_bootstrap.cpp     Vulkan 设备初始化
    vn_decoder.cpp       命令解码 + 真实 Vulkan 执行
    win_capture.cpp      画面捕获 + LZ4 压缩回传

scripts/
  disasm_cmdstream.py    命令流反汇编工具

tests/
  dx11_triangle/         静态三角形测试
  dx11_depth_test/       深度测试
  dx11_multi_blend/      多物体 + alpha blend
  SortTheCourt/          Unity 5.3 游戏测试
  UltraKill/             Unity 游戏测试
```

## 构建

需要 Windows 10/11 + Visual Studio 2022 + Vulkan SDK 1.3.216.0+。

```bash
# 64-bit（Host server + 64-bit ICD）
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Debug --target vbox_host_server
cmake --build build --config Debug --target vbox_vk_icd

# 32-bit ICD（SortTheCourt 等 PE32 游戏需要）
cmake -S . -B build32 -G "Visual Studio 17 2022" -A Win32
cmake --build build32 --config Debug --target vbox_vk_icd
```

## 运行（SortTheCourt 示例）

```bash
# 1. 部署 32-bit ICD
cp build32/guest_vk_icd/Debug/vbox_vulkan.dll tests/SortTheCourt/

# 2. 启动 Host server
build/host/Debug/vbox_host_server.exe

# 3. 启动游戏（新终端）
export VK_ICD_FILENAMES='S:\bld\vboxgpu\tests\SortTheCourt\vbox_icd.json'
export VK_LOADER_LAYERS_DISABLE='*'
cd tests/SortTheCourt && ./SortTheCourt.exe -screen-width 800 -screen-height 600
```

## 调试工具

```bash
# 录制命令流 dump
vbox_host_server.exe --dump dumps/sc.bin

# 回放 dump（不需要游戏）
vbox_host_server.exe --replay dumps/sc.bin

# 反汇编命令流
python3 scripts/disasm_cmdstream.py dumps/sc.bin --max-batches 5
```

## 许可证

GPLv3（与 VirtualBox 一致）
