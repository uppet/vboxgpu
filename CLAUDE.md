# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

**VBox GPU Bridge** — 为 VirtualBox 中的 Windows 7 虚拟机提供 DX11 级别 3D 加速。

核心链路：Guest DX11 → DXVK (DX11→Vulkan) → Venus 协议序列化 → 传输通道 → Host Venus 解码 → Host Vulkan GPU 渲染。

## 项目状态

**阶段一 M1.2 进行中** — ICD 代理 + DXVK 集成。

已完成（M1.2 里程碑达成）：
- DX11 三角形通过 DXVK → ICD → TCP → Host Vulkan 完整链路渲染并在窗口显示 ✓
- ICD 代理框架（Vulkan 1.3, Features2, 60+ 扩展）
- Venus 命令流编解码 + TCP 传输 + 延迟 Present
- GPU 资源序列化：Image/Memory/ImageView/Sampler/DescriptorSet 全链路
- Descriptor 路径：DescriptorUpdateTemplate → UpdateDescriptorSets + BindDescriptorSets2
- Pipeline barrier 转发（vkCmdPipelineBarrier/Barrier2）
- BeginRendering imageView 路由（swapchain vs 内部 render target）
- 命令流反汇编工具 + Windows.Graphics.Capture 窗口截图

当前状态：
- DX11 三角形能在 Host 窗口正确显示（用户已修改颜色值验证）
- DXVK 原始 SPIR-V shader 渲染，非 builtin shader

后续：
- 清理调试代码和日志
- 更多 DX11 测试用例（带纹理、vertex buffer）
- Buffer 数据传输（MapMemory → Host GPU）

## 架构要点

项目分四个阶段实施：

1. **阶段一：DLL 代理验证** — Guest 内用魔改 DXVK 的 d3d11.dll 代理拦截 DX11 调用，经 Venus 编码后通过 TCP/HGCM 发送到 Host，Host 在独立窗口显示渲染结果
2. **阶段二：WDDM 驱动下沉** — 将翻译逻辑移入 WDDM UMD (vboxgpu_umd.dll)，游戏使用微软原版 d3d11.dll，KMD (vboxgpu_kmd.sys) 只做命令搬运
3. **阶段三：VBox 深度集成** — 添加虚拟 PCI 设备 (BAR0 控制/BAR1 命令 Ring Buffer/BAR2 数据传输)，共享内存传输
4. **阶段四：性能优化与游戏兼容**

关键设计决策：
- DXVK 魔改点在 `DxvkDevice` 层，用 `RemoteVkDevice` 替换真实 Vulkan 设备，将调用路由到 Venus Encoder
- Shader 编译 (DXBC→SPIR-V) 在 Guest 侧完成，SPIR-V 字节码直接序列化传输
- 阶段一/二的 Host 侧代码完全共用

## 构建系统（规划）

| 组件 | 构建系统 |
|------|---------|
| dxvk-remote | Meson (cross-compile) |
| venus-encoder/decoder | CMake |
| Guest KMD | WDK (build/nmake) |
| Guest UMD / Host 后端 | CMake + MSVC |
| VBox 虚拟设备 | kBuild |

## 开发环境

- 开发在 WSL2 (Ubuntu) 中进行，通过 MCP 工具与 Windows 侧交互
- Host: Windows 10/11, Visual Studio 2022, Vulkan SDK, CMake 3.25+
- Guest: VirtualBox 7.x, Windows 7 SP1 x64, Guest Additions, DirectX SDK
- 阶段二: WDK 7.1, WinDbg
- 阶段三: VirtualBox 源码 + kBuild + Qt 5.x

## MCP 工具约定

- **delegate-runner** — 用于在 Windows 侧执行构建任务（CMake 配置、MSVC 编译等）
- **mcp-horse** — 用于操作浏览器，完成检索或 curl 无法实现的资源获取

## 核心依赖

| 项目 | 用途 | 许可证 |
|------|------|--------|
| DXVK | DX11→Vulkan 翻译 | zlib |
| venus-protocol | 命令序列化协议 | MIT |
| virglrenderer | Venus 解码 + Host Vulkan 执行 | MIT |
| Gfxstream | Vulkan 编解码生成器, Ring Buffer | Apache 2.0 |

整体许可证：GPLv3（与 VirtualBox 一致）。

## 调试策略

- 命令流录制/回放：传输层插入钩子 dump 命令流，Host 独立回放（不需要 VM）
- Host Vulkan：RenderDoc + Validation Layers 截帧分析
- DXVK：Visual Studio 附加进程 + DXVK_LOG
- KMD：WinDbg 通过 VBox 串口内核调试

## 代码提交

- 如果 Claude 模型参与修改，署名加上 `Co-Authored-By: Claude`（不补充邮件地址）
