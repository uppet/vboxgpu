# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

**VBox GPU Bridge** — 为 VirtualBox 中的 Windows 7 虚拟机提供 DX11 级别 3D 加速。

核心链路：Guest DX11 → DXVK (DX11→Vulkan) → Venus 协议序列化 → 传输通道 → Host Venus 解码 → Host Vulkan GPU 渲染。

## 项目状态

**阶段一 M1.6 基本完成** — 真实 Unity 游戏 (SortTheCourt) 通过完整链路 60 FPS 可玩运行。

已完成里程碑：
- M1.2：DX11 静态三角形通过 DXVK → ICD → TCP → Host Vulkan 完整链路渲染 ✓
- M1.3：变色动画三角形（cbuffer 传 time 值 + HSV 色环 shader）✓
- M1.4：纹理三角形渲染（vertex buffer + texture + sampler 全链路）✓
- M1.5：深度测试 + 多物体 + alpha blend + blend state 切换 + codegen Stage 3 ✓
- M1.6：SortTheCourt (Unity 5.3, 32-bit) 60 FPS 可玩 + UltraKill 画面渲染正常 ✓

已完成功能：
- ICD 代理框架（Vulkan 1.3, Features2, 60+ 扩展）
- Venus 命令流编解码 + TCP 传输 + 延迟 Present
- GPU 资源序列化：Image/Memory/ImageView/Sampler/DescriptorSet/Buffer 全链路
- Shadow memory persistent mapping（per-memory shadow buffer）
- Buffer 数据传输：MapMemory → shadow → WriteMemory → Host GPU
- Uniform buffer descriptor 绑定（UpdateDescriptorSets + BindDescriptorSets）
- Descriptor 路径：DescriptorUpdateTemplate → UpdateDescriptorSets + BindDescriptorSets2
- Pipeline barrier 转发（vkCmdPipelineBarrier/Barrier2）
- ClearAttachments 转发
- BeginRendering imageView 路由（swapchain vs 内部 render target）
- Viewport 动态状态转发（含 Y 轴翻转）
- CullMode / FrontFace 动态状态转发
- 命令流反汇编工具 + Host 内置 captureScreenshot
- Venus protocol codegen 框架（59/67 API 可自动生成，7 个已集成到编解码链路）
- 资源销毁全链路（15 个 vkDestroy*/vkFreeMemory 命令，host 端正确释放）
- Present fence 转发（VkSwapchainPresentFenceInfoKHR pNext 解析）
- Host server 常驻多客户端架构（accept loop + ClientSession，每个连接独立 VkDevice/窗口/线程）
- 命令 ID 全量迁移到 Venus VkCommandTypeEXT 标准值
- 深度测试全链路（5 个 depth 动态状态命令 + BeginRendering depth attachment + pipeline depth/stencil state）
- Alpha blend 支持（pipeline blend state 转发，blend enable/factors/op/mask）
- 多物体渲染（per-object constant buffer update between draws）
- BDA (BufferDeviceAddress) 地址转发（同步查询 Host 真实 GPU 地址，DXVK 2.7.1 必需）
- 32-bit ICD 构建支持（build32 目录，PE32 游戏可加载）
- 画面回传（Host→Guest LZ4 压缩帧 + GDI StretchDIBits 贴图）
- 大 mapped memory 传输（descriptor heap 等 16MB+ 区域全量 flush）
- 性能优化：双缓冲 readback + 异步 LZ4 压缩 + GDI 限速 60 FPS
- 性能修复：删除 icd_vkCmdBindVertexBuffers2 的 explicit flush（44MB→270KB/帧）
- 性能修复：删除 decoder 热路径 fprintf（800ms→9ms 批次解码）
- BDA memory 白名单 + 线程池 async patching（纹理 batch 3585ms→16ms）
- WriteMemory UAF 修复（readBytes 同步拷贝，不存储 batch buffer 指针）

当前状态（M1.6 完成）：
- **SortTheCourt（Unity 5.3, 32-bit）Host 窗口 + Guest 回传 60 FPS 可玩** ✓
- **UltraKill（Unity, 64-bit）Loading screen ~60 FPS，实际游玩 ~20 FPS，有零星渲染错误** ⚠️
- 既有测试用例全部通过（dx11_triangle, dx11_depth_test, dx11_multi_blend）

## 性能状态（重要）

**SortTheCourt 稳定帧率：60-67 FPS**（受限于显示器刷新率）
- 批次大小：加载期 29-44MB（纹理上传，正常），稳定期 ~270KB/帧
- Host 解码时间：稳定期 ~9ms/帧
- 两个已解决的性能陷阱：
  1. `icd_vkCmdBindVertexBuffers2` explicit flush：每次 BindVB 触发 4MB WriteMemory，11 次/帧 = 44MB 无效流量
  2. decoder 热路径 fprintf：每批 400+ 次 fprintf 通过 delegate-runner pipe 产生阻塞，WM dispatch 膨胀 ~93ms

## Decoder 调试日志说明

- 热路径 fprintf 已删除（BarrierDrawIndexedBindVB 等），默认静默
- 编译时加 `-DVBOXGPU_VERBOSE` 可开启所有日志（仅调试用，会严重影响性能）
- 批次级 profiling 日志（`[Batch#]` + `[type=0x...]`）保留，仅对 >1MB 批次输出
- 错误路径日志（`FAILED`、`SKIP` 等）始终保留

M1.7 / 后续目标：

性能优化：
- **decode/GPU 流水线化（Method-A）**：当前 host 串行处理 batch（recv→decode→GPU→readback→send），GPU 和 CPU 互相等待。双缓冲流水线：decode batch N+1 同时 GPU 执行 batch N，预期提升 30-50% FPS
- **BDA patching 按需 skip**：本机运行时 liveBdaToReplayBda_ 中 live==replay，所有 BDA scan 写入值等于原值。检测此条件后整个 BDA scan 可跳过
- Mapped memory 增量传输（dirty tracking，当前全量 flush 可以满足正确性）

功能完善：
- Stencil test 支持
- Mipmap 支持
- 多 Render Target (MRT)
- 更多游戏兼容性测试

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

Host server 架构（多客户端服务）：
- `main_server.cpp`：主进程 — accept loop（线程）+ dashboard 窗口消息泵（主线程）
- `client_session.h/cpp`：每个 TCP 连接一个 ClientSession 实例，包含独立 VkDevice + VnDecoder + 渲染窗口 + worker/compress 线程
- 支持 live（TCP）和 replay（文件）两种 session 模式
- `--dump file.bin`：录制命令流；`--replay file.bin`：回放命令流
- 最大并发数由 `VBOXGPU_MAX_SESSIONS` 环境变量控制（默认 3）
- Dashboard UI 显示运行时间、端口、session 列表；checkbox 控制渲染窗口 cloak
- 辅助脚本：`man_run.bat`（构建+启动）、`man_run_only.bat`（只启动 guest）、`kill_host.bat`（停止 host）

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

- 命令流录制：`host.exe --dump file.bin`，录制第一个连接的 TCP 命令流
- 命令流回放：`host.exe --replay file.bin [--save-frames dir]`，复用 ClientSession 回放
- Host Vulkan：RenderDoc + Validation Layers 截帧分析
- DXVK：Visual Studio 附加进程 + DXVK_LOG
- KMD：WinDbg 通过 VBox 串口内核调试

## 代码提交

- 如果 Claude 模型参与修改，署名加上 `Co-Authored-By: Claude`（不补充邮件地址）
