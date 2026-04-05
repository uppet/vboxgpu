# VBox GPU Bridge

为 VirtualBox Windows 7 虚拟机提供 DX11 级别 3D 加速。

## 工作原理

```
Guest (VM)                          Host (物理机)
┌─────────────────┐                ┌──────────────────┐
│ DX11 App        │                │                  │
│   ↓             │                │  Venus Decoder   │
│ DXVK (DX11→VK)  │    TCP/HGCM   │   ↓              │
│   ↓             │ ─────────────→ │  Real Vulkan GPU │
│ ICD Proxy       │  命令流        │   ↓              │
│ (vbox_vulkan.dll)│               │  窗口显示        │
└─────────────────┘                └──────────────────┘
```

Guest 侧的 DXVK 将 DX11 调用翻译为 Vulkan，经我们的 ICD 代理拦截后序列化为 Venus 命令流，通过 TCP 传输到 Host，Host 解码后调用真实 Vulkan GPU 渲染。

## 当前状态

**阶段一 (M1.2)** — ICD 代理 + DXVK 集成，基本渲染链路已通。

- [x] Vulkan ICD 代理框架（欺骗 DXVK 完成初始化）
- [x] Venus 命令流编解码
- [x] TCP 传输层
- [x] Vulkan 1.3 Dynamic Rendering 支持
- [x] Host 端渲染窗口 + Swapchain 管理
- [x] 端到端三角形渲染验证通过
- [ ] DXVK 原始 SPIR-V shader 支持（需要 pipeline layout 转发）
- [ ] GPU 资源（Buffer/Image/Memory）序列化

## 构建

需要 Windows 10/11 + Visual Studio 2022 + Vulkan SDK 1.3.216.0+。

```bash
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Debug
```

## 运行

```bash
# 1. 启动 Host 渲染服务
build/host/Debug/vbox_host_server.exe

# 2. 运行 DX11 测试程序（需要 DXVK 的 d3d11.dll 在 test_env 中）
set VK_ICD_FILENAMES=tests/dx11_triangle/test_env/vbox_icd.json
set VK_LOADER_LAYERS_DISABLE=*
tests/dx11_triangle/test_env/dx11_triangle.exe
```

## 项目结构

```
common/
  transport/          TCP 传输层
  venus/              命令编解码 (vn_command.h, vn_encoder.h, vn_stream.h)

guest_vk_icd/         Vulkan ICD 代理 → vbox_vulkan.dll
guest_sim/            命令流模拟器（调试用）

host/
  src/
    main_server.cpp   TCP server 模式入口
    main_cmdstream.cpp 命令流模式入口（含 replay）
    vk_bootstrap.cpp  Vulkan 设备初始化
    vn_decoder.cpp    命令解码 + 真实 Vulkan 执行

tests/
  dx11_triangle/      DX11 三角形测试程序
```

## 许可证

GPLv3（与 VirtualBox 一致）
