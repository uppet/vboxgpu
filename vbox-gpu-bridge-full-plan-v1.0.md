# VBox GPU Bridge — 完整项目方案

## 为 VirtualBox Win7 虚拟机提供 DX11 级别 3D 加速

### 版本 1.0 | 2026.03

---

## 目录

1. 项目概述与目标
2. 核心技术决策
3. 整体架构
4. 分阶段实施路线
5. 阶段一：DLL 代理验证（详细）
6. 阶段二：WDDM 驱动下沉（详细）
7. 阶段三：VirtualBox 深度集成
8. 阶段四：性能优化与游戏兼容
9. 可复用的开源资源清单
10. 关键技术风险与应对
11. 项目结构与仓库规划
12. 调试策略
13. 开源与社区策略

---

## 1. 项目概述与目标

### 1.1 项目愿景

在 Windows 宿主机上的 VirtualBox 中，为 Windows 7 虚拟机提供 DirectX 11 级别的现代 3D 图形加速能力。

### 1.2 为什么做这个项目

当前行业中没有任何方案能覆盖 "Windows Host + Win7 Guest + DX11 3D 加速" 这个组合：

- VirtualBox 现有 3D 加速仅支持 DX9 / OpenGL 2.1
- Hyper-V GPU-PV 不支持 Win7 Guest
- VFIO GPU Passthrough 需要 Linux Host
- VMware Workstation 的 3D 支持有限且非开源
- QEMU + Venus 目前仅支持 Linux Guest

### 1.3 核心思路

借鉴游戏主机模拟器的图形后端架构，在应用层面做 API 级别的翻译和远程执行：

1. Guest 内的 DX11 调用通过 DXVK 翻译为 Vulkan 命令
2. Vulkan 命令通过 Venus 协议序列化为二进制命令流
3. 命令流通过传输通道发送到 Host
4. Host 端解码后在真实 GPU 上通过 Vulkan 执行渲染
5. 渲染结果回传给 Guest 显示

### 1.4 关键技术决策总结

| 决策点 | 选择 | 理由 |
|--------|------|------|
| DX 翻译方案 | 复用 DXVK | 生产级成熟度，支持 80%+ DX11 游戏 |
| 命令传输协议 | 复用 Venus 协议 | 已有完整序列化/反序列化实现，无需自研 |
| Host 渲染 API | Vulkan | 跨平台，性能最优，与 DXVK/Venus 天然配合 |
| 验证策略 | 先 DLL 代理，后驱动下沉 | 快速验证通路，Host 侧代码两阶段完全共用 |
| Guest 驱动模型 | WDDM 1.1 (阶段二) | Win7 原生支持，游戏完全透明 |
| 帧回传方案 | 先 Host 窗口显示，后共享内存 | 降低初期复杂度 |

---

## 2. 核心技术决策详解

### 2.1 为什么选择 DXVK + Venus 而非自研协议

自研一套 DX11 命令序列化协议需要覆盖约 200+ 个 DDI 回调、数百种资源格式、完整的 Shader 编译管线。这些工作 DXVK 和 Venus 已经分别解决了：

- **DXVK** 解决 DX11→Vulkan 翻译：所有 DX11 状态、资源、Shader 的 Vulkan 等价映射
- **Venus** 解决 Vulkan 命令序列化：所有 Vulkan API 调用的二进制编解码
- **两者结合** 形成完整链路：DX11 → Vulkan → 二进制流 → 传输 → Vulkan → GPU

我们的原创工作集中在"胶合层"和"传输层"，而非翻译逻辑本身。

### 2.2 为什么先用 DLL 代理再下沉到驱动

| 维度 | 阶段一：DLL 代理 | 阶段二：WDDM 驱动 |
|------|-----------------|-------------------|
| 开发环境 | 普通 C++ IDE | WDK + 内核调试 |
| 编译时间 | 秒级 | 分钟级（VBox 源码）|
| 调试方式 | Visual Studio 附加进程 | WinDbg 内核调试 |
| 部署方式 | 复制 DLL 到游戏目录 | 安装驱动 + 重启 |
| 驱动签名 | 不需要 | 需要（测试模式可绕过）|
| 内核蓝屏风险 | 无 | 有 |
| 游戏兼容性 | 部分游戏有反作弊检测 | 完全透明 |
| 迭代速度 | 快 | 慢 |

**关键点：两个阶段的 Host 侧代码完全相同，Guest 侧的 DXVK 翻译逻辑和 Venus 编码逻辑也相同，只是承载位置不同（独立 DLL vs WDDM UMD）。阶段一的工作不会浪费。**

### 2.3 Windows 图形栈中的切入层级

```
                        阶段一切入点          阶段二切入点
                             │                     │
                             ▼                     │
Game.exe ──► d3d11.dll ──► [DXVK 代理]             │
                  │                                │
                  │    （微软原版 d3d11.dll）         │
                  │              │                  │
                  │         dxgi.dll                │
                  │              │                  │
                  │         ┌────▼────┐             │
                  │         │  UMD    │ ◄───────────┘
                  │         └────┬────┘
                  │              │
                  │         ┌────▼────┐
                  │         │  KMD    │
                  │         └────┬────┘
                  │              │
                  └──────────────┼──── 传输通道 ────► Host
```

---

## 3. 整体架构

### 3.1 端到端数据流

```
┌─────────────────── Guest (Windows 7) ───────────────────────┐
│                                                              │
│  Game.exe                                                    │
│    │  DirectX 11 API 调用                                    │
│    ▼                                                         │
│  ┌──────────────────────────────────────────────────┐        │
│  │  DXVK 翻译层                                     │        │
│  │  (阶段一: 自定义 d3d11.dll)                       │        │
│  │  (阶段二: WDDM UMD 内嵌)                         │        │
│  │                                                   │        │
│  │  - DX11 渲染状态 → Vulkan Pipeline State          │        │
│  │  - DXBC Shader → SPIR-V (Shader 编译器)          │        │
│  │  - DX11 资源 → Vulkan Image/Buffer               │        │
│  │  - Draw/Dispatch → vkCmd* 命令                    │        │
│  └──────────────────┬───────────────────────────────┘        │
│                     │  Vulkan API 调用（不直接执行）           │
│                     ▼                                        │
│  ┌──────────────────────────────────────────────────┐        │
│  │  Venus Encoder（命令序列化）                       │        │
│  │                                                   │        │
│  │  - vkCreateGraphicsPipelines → 二进制编码         │        │
│  │  - vkCmdDraw → 二进制编码                         │        │
│  │  - vkAllocateMemory → 二进制编码                  │        │
│  │  - SPIR-V Shader 字节码直接打包                    │        │
│  │  - 纹理数据批量打包                                │        │
│  └──────────────────┬───────────────────────────────┘        │
│                     │  二进制命令流                            │
│                     ▼                                        │
│  ┌──────────────────────────────────────────────────┐        │
│  │  传输层                                           │        │
│  │  (阶段一: VBoxGuest HGCM / TCP)                  │        │
│  │  (阶段二: 虚拟 PCI BAR 共享内存)                  │        │
│  │  (阶段三: VirtualBox 深度集成)                    │        │
│  └──────────────────┬───────────────────────────────┘        │
│                     │                                        │
├─────────────────────┼────────────────────────────────────────┤
│                     │  Guest ←→ Host 边界                    │
├─────────────────────┼────────────────────────────────────────┤
│                     ▼                                        │
│  ┌──────────────────────────────────────────────────┐        │
│  │  Venus Decoder（命令反序列化）                     │        │
│  │                                                   │        │
│  │  - 二进制流 → Vulkan API 调用                     │        │
│  │  - 资源 ID 映射 (Guest ID → Host VkObject)       │        │
│  │  - SPIR-V 字节码 → VkShaderModule                │        │
│  └──────────────────┬───────────────────────────────┘        │
│                     │  标准 Vulkan API 调用                   │
│                     ▼                                        │
│  ┌──────────────────────────────────────────────────┐        │
│  │  宿主机 Vulkan 驱动                               │        │
│  │  (NVIDIA / AMD / Intel 原生驱动)                  │        │
│  └──────────────────┬───────────────────────────────┘        │
│                     │                                        │
│                     ▼                                        │
│               宿主机真实 GPU                                  │
│                     │                                        │
│                     ▼                                        │
│  ┌──────────────────────────────────────────────────┐        │
│  │  帧交付                                           │        │
│  │  (阶段一: Host 窗口直接显示)                      │        │
│  │  (阶段二: 共享内存回传到 Guest 帧缓冲)            │        │
│  └──────────────────────────────────────────────────┘        │
│                                                              │
│                     Host (Windows 10/11)                      │
└──────────────────────────────────────────────────────────────┘
```

### 3.2 组件清单

| 组件 | 位置 | 语言 | 阶段 | 来源 |
|------|------|------|------|------|
| DXVK Core (Shader 编译+状态映射) | Guest 用户态 | C++ | 一 | 复用 + 魔改 |
| Venus Encoder | Guest 用户态 | C | 一 | 复用 + 移植 |
| Guest 传输层 | Guest 用户态/内核态 | C/C++ | 一/二 | 原创 |
| WDDM KMD | Guest 内核态 | C | 二 | 原创(参考 virtio-win) |
| WDDM UMD | Guest 用户态 | C++ | 二 | 原创(内嵌 DXVK+Venus) |
| VirtualBox 虚拟 PCI 设备 | Host VBox 进程 | C++ | 二 | 原创(参考 VMSVGA) |
| Host 传输层 | Host VBox 进程 | C++ | 一/二 | 原创 |
| Venus Decoder | Host 用户态 | C | 一 | 复用 + 移植 |
| Host Vulkan 执行 | Host 用户态 | C | 一 | 复用(virglrenderer) |
| 帧交付 | Host 用户态 | C++ | 一/二 | 原创(参考 Looking Glass) |

---

## 4. 分阶段实施路线

```
阶段一                阶段二                阶段三              阶段四
DLL代理验证            WDDM驱动下沉          VBox深度集成         性能与兼容
(3-4个月)             (3-5个月)             (2-3个月)           (持续)
│                     │                     │                   │
├─ M1.1 Host渲染独立   ├─ M2.1 最小WDDM驱动  ├─ M3.1 虚拟PCI设备  ├─ 命令批量优化
│  验证(3周)           │  加载(6周)           │  (6周)            ├─ Shader缓存
│                     │                     │                   ├─ 纹理压缩传输
├─ M1.2 DXVK魔改      ├─ M2.2 UMD集成       ├─ M3.2 共享内存     ├─ 帧回传优化
│  +Venus移植(6周)     │  DXVK+Venus(8周)    │  传输(4周)        ├─ 游戏兼容修复
│                     │                     │                   │
├─ M1.3 传输层        ├─ M2.3 KMD传输       ├─ M3.3 GUI集成     ├─ 打包为
│  +端到端(4周)        │  +帧回传(6周)        │  (2周)            │  Extension Pack
│                     │                     │                   │
▼                     ▼                     ▼                   ▼
验收：DX11三角形       验收：原版d3d11        验收：无需           验收：Portal 2
在Host窗口显示         可跑简单DX11程序       额外配置即可用       等游戏30+FPS
```

---

## 5. 阶段一：DLL 代理验证（详细）

### 5.1 阶段目标

在 Guest Win7 中运行一个 DX11 程序（三角形 demo），通过 DXVK→Venus→传输→Host Vulkan 的完整链路，在 Host 的一个窗口中正确显示渲染结果。

**不需要：** 编译 VirtualBox 源码、写内核驱动、处理驱动签名。

### 5.2 里程碑 1.1：Host 渲染引擎独立验证（3 周）

**目标：** 在 Host 上独立运行一个程序，手工构造 Venus 命令流，用 Vulkan 渲染出三角形。

**工作内容：**

1. 搭建开发环境
   - Host: Windows 10/11, Vulkan SDK, Visual Studio 2022
   - 编译 virglrenderer 的 Venus 后端（需移植到 Windows）
   - 准备 Vulkan 验证层

2. 实现最小 Venus 命令消费者
   ```
   读取命令流文件 → Venus Decoder 解码 → Vulkan API 调用 → 窗口显示
   ```

3. 手工构造测试命令流
   - VkCreateInstance
   - VkCreateDevice
   - VkCreateRenderPass
   - VkCreateShaderModule (内嵌预编译的 SPIR-V 三角形 Shader)
   - VkCreateGraphicsPipeline
   - VkAllocateMemory + VkCreateBuffer (三角形顶点)
   - VkCmdBeginRenderPass + VkCmdDraw + VkCmdEndRenderPass
   - VkQueueSubmit + VkQueuePresentKHR

**验收标准：** Host 程序读取命令流文件，渲染出彩色三角形。

**关键输出：** 确认 Venus 解码器在 Windows 上能正确工作，确认 Vulkan 后端渲染管线通畅。

### 5.3 里程碑 1.2：DXVK 魔改 + Venus 编码器移植（6 周）

**目标：** 魔改 DXVK，使其输出 Venus 命令流而非直接调用 Vulkan。

**5.3.1 DXVK 魔改方案**

DXVK 的架构中有一个关键抽象层 `DxvkDevice`，它封装了所有 Vulkan 设备操作。我们的魔改策略是在这一层做拦截：

```
DXVK 原始模式:
  DX11 DDI 回调 → DXVK Core 翻译 → DxvkDevice → Vulkan API → 本地 GPU

DXVK 魔改模式（我们的方案）:
  DX11 DDI 回调 → DXVK Core 翻译 → DxvkDevice → Venus Encoder → 命令流输出
```

具体改动点：

**a) Vulkan 调用拦截**

DXVK 的所有 Vulkan 调用都通过 `DxvkDevice` 和 `DxvkCommandList` 发出。我们需要：

- 创建一个 `RemoteVkDevice` 实现，替换 `DxvkDevice` 中的真实 Vulkan 设备
- `RemoteVkDevice` 不调用真实的 vkCreateBuffer 等函数，而是调用 Venus Encoder 将调用序列化
- 对于需要返回值的调用（如 vkAllocateMemory 返回 VkDeviceMemory），使用同步等待 Host 返回或本地生成代理句柄

**b) 需要特殊处理的 Vulkan 调用类别**

| 类别 | 示例 | 处理方式 |
|------|------|---------|
| 对象创建 | vkCreateBuffer, vkCreateImage | 序列化参数，Host 创建真实对象，返回 ID |
| 命令录制 | vkCmdDraw, vkCmdBindPipeline | 纯序列化，无需等待返回 |
| 内存映射 | vkMapMemory | 需要在 Guest 侧分配影子内存，写入时标记脏页 |
| 同步操作 | vkWaitForFences, vkDeviceWaitIdle | 需要实际等待 Host 完成 |
| 查询操作 | vkGetQueryPoolResults | 需要从 Host 回读结果 |
| Shader 创建 | vkCreateShaderModule | SPIR-V 字节码直接序列化传输 |
| Pipeline 创建 | vkCreateGraphicsPipelines | 序列化所有状态，Host 创建 |

**c) Shader 处理流程**

```
游戏提交 DXBC Shader
     │
     ▼
DXVK 的 DxbcCompiler 编译为 SPIR-V（这步在 Guest 内完成）
     │
     ▼
SPIR-V 字节码通过 Venus 序列化传输到 Host
     │
     ▼
Host 调用 vkCreateShaderModule 创建 Shader
     │
     ▼
返回 Shader ID 给 Guest，后续绑定时使用此 ID
```

Shader 编译放在 Guest 侧的好处是：Host 侧不需要 DXBC 编译器，只需要标准的 Vulkan 驱动。坏处是 Guest 的 CPU 开销较大，但 Shader 编译结果可以缓存。

**5.3.2 Venus 编码器 Windows 移植**

Venus 编码器在 Mesa 源码的 `src/virtio/vulkan/` 目录下。需要的移植工作：

| Linux 依赖 | Windows 替代 | 工作量 |
|-----------|-------------|--------|
| fd (文件描述符) | HANDLE | 低 |
| mmap / munmap | VirtualAlloc / MapViewOfFile | 低 |
| pthread_mutex | CRITICAL_SECTION / SRWLock | 低 |
| drm / dma-buf | 不需要（我们用自己的传输层） | 无 |
| virtio-gpu 传输 | 替换为自定义传输层 | 中 |

核心编解码逻辑是纯 C 代码，没有平台依赖，移植工作主要在传输层适配。

**5.3.3 最小可验证的 DXVK 魔改范围**

不需要一次性支持所有 Vulkan API。第一版只需要支持渲染一个三角形所需的最小 API 集合：

```
必须支持（约 30 个 API）:
  实例/设备: vkCreateInstance, vkCreateDevice, vkGetPhysicalDevice*
  内存: vkAllocateMemory, vkFreeMemory, vkMapMemory, vkUnmapMemory
  缓冲区: vkCreateBuffer, vkDestroyBuffer, vkBindBufferMemory
  图像: vkCreateImage, vkDestroyImage, vkBindImageMemory, vkCreateImageView
  Shader: vkCreateShaderModule
  Pipeline: vkCreateGraphicsPipelines, vkCreatePipelineLayout
  渲染: vkCreateRenderPass, vkCreateFramebuffer
  命令: vkCreateCommandPool, vkAllocateCommandBuffers,
        vkBeginCommandBuffer, vkEndCommandBuffer,
        vkCmdBeginRenderPass, vkCmdEndRenderPass,
        vkCmdBindPipeline, vkCmdBindVertexBuffers, vkCmdDraw
  提交: vkQueueSubmit
  同步: vkCreateFence, vkWaitForFences
  呈现: vkCreateSwapchainKHR (由 Host 窗口管理代替)

暂不支持（后续增量添加）:
  Compute Shader, 纹理采样, 混合状态, 深度测试, MSAA,
  查询对象, 间接绘制, 多渲染目标, ...
```

**验收标准：** 在 Guest Win7 中运行魔改 DXVK 版本的 DX11 三角形程序，DXVK 成功将 DX11 调用翻译为 Venus 命令流并写入文件（或 socket）。

### 5.4 里程碑 1.3：传输层 + 端到端验证（4 周）

**目标：** 打通 Guest→Host 的传输通道，实现端到端渲染。

**5.4.1 传输方案选择**

阶段一有三个传输方案可选，按实现难度排序：

**方案 A：TCP Socket（最简单，推荐先用）**

```
Guest: DXVK → Venus Encoder → TCP Client (连接 Host IP:Port)
Host:  TCP Server → Venus Decoder → Vulkan → 窗口显示
```

VirtualBox 的 NAT 网络或桥接网络都支持 Guest→Host 的 TCP 连接。延迟较高（约 0.5-1ms per roundtrip），帧率会很低，但足以验证正确性。

**方案 B：VirtualBox HGCM（推荐第二步切换）**

HGCM（Host-Guest Communication Manager）是 VirtualBox Guest Additions 提供的通信机制，共享文件夹和剪贴板就用的这个。

```
Guest: DXVK → Venus Encoder → VBoxGuest.sys HGCM 调用
Host:  VBoxService HGCM Handler → Venus Decoder → Vulkan
```

不需要编译 VirtualBox 源码，用现有的 Guest Additions API 即可。性能比 TCP 好，但仍有系统调用开销。

**方案 C：共享内存（阶段二/三使用）**

需要修改 VirtualBox 源码添加虚拟 PCI 设备。阶段一不使用。

**5.4.2 帧显示方案**

阶段一不做帧回传到 Guest，而是在 Host 上开一个窗口直接显示渲染结果（类似 Looking Glass 的模式）。

```
Host 渲染线程:
  Venus Decoder → Vulkan 渲染 → VkSwapchain → Host 窗口显示
```

Guest 虚拟机的屏幕上不会显示游戏画面（仍然是普通桌面），游戏画面出现在 Host 的独立窗口中。这在验证阶段完全可以接受。

**5.4.3 同步与流控**

命令流是异步的（Guest 持续写入，Host 持续消费），但某些操作需要同步：

- **Fence 等待：** Guest 调用 vkWaitForFences 时，需要等待 Host 确认对应的渲染命令已完成
- **内存映射读回：** Guest 调用 vkMapMemory 读取 GPU 数据时，需要从 Host 拿到数据
- **查询结果：** vkGetQueryPoolResults 需要从 Host 回读

同步机制在阶段一可以用简单的请求-响应模式：Guest 发送同步请求，阻塞等待 Host 回复。

**验收标准：** Guest Win7 中运行 DX11 三角形 demo，Host 窗口中显示正确渲染的彩色三角形。端到端通路完全打通。

### 5.5 阶段一交付物

```
vbox-gpu-bridge/
├── guest/
│   ├── dxvk-remote/              # DXVK 魔改版本
│   │   ├── src/dxvk/
│   │   │   ├── dxvk_remote_device.h/cpp   # RemoteVkDevice 实现
│   │   │   └── dxvk_remote_cmdlist.h/cpp  # 远程命令录制
│   │   └── src/d3d11/                     # DX11 翻译（基本不改）
│   │
│   ├── venus-encoder/            # Venus 编码器 Windows 移植
│   │   ├── vn_encode_*.c         # 从 Mesa 移植的编码函数
│   │   └── vn_transport_win.c    # Windows 传输层适配
│   │
│   └── transport/
│       ├── tcp_client.cpp        # TCP 传输（方案 A）
│       └── hgcm_client.cpp       # HGCM 传输（方案 B）
│
├── host/
│   ├── venus-decoder/            # Venus 解码器 Windows 移植
│   │   ├── vn_decode_*.c         # 从 virglrenderer 移植
│   │   └── vn_dispatch.c         # 解码→Vulkan 调用分发
│   │
│   ├── vulkan-backend/           # Vulkan 渲染后端
│   │   ├── vk_instance.cpp       # Vulkan 初始化
│   │   ├── vk_resource_map.cpp   # Guest资源ID→VkObject 映射
│   │   └── vk_window.cpp         # Host 窗口 + VkSwapchain
│   │
│   └── transport/
│       ├── tcp_server.cpp        # TCP 传输
│       └── hgcm_server.cpp       # HGCM 传输
│
├── common/
│   ├── venus_protocol.h          # Venus 协议定义（来自 venus-protocol）
│   └── transport_interface.h     # 传输层抽象接口
│
├── tests/
│   ├── dx11_triangle/            # DX11 三角形测试程序
│   ├── dx11_textured_quad/       # DX11 纹理四边形
│   └── command_dump/             # 命令流录制/回放工具
│
└── docs/
    ├── BUILD.md                  # 构建指南
    └── ARCHITECTURE.md           # 架构说明
```

---

## 6. 阶段二：WDDM 驱动下沉（详细）

### 6.1 阶段目标

将阶段一验证通过的翻译逻辑从 d3d11.dll 代理下沉到 WDDM 驱动层，实现：

- 游戏使用微软原版 d3d11.dll，完全无感知
- 系统级生效，所有 DX11 应用自动加速
- DWM 桌面合成也能加速

### 6.2 WDDM 驱动架构

```
d3d11.dll (微软原版)
     │
     │  DDI 回调 (约 200 个函数指针)
     ▼
┌────────────────────────────────────────────────┐
│  UMD: vboxgpu_umd.dll (用户态)                  │
│                                                  │
│  ┌─────────────────┐  ┌──────────────────────┐  │
│  │  DDI 入口层      │  │  DXVK Core           │  │
│  │  (接收 d3d11.dll │──│  (DX11→Vulkan 翻译)  │  │
│  │   的 DDI 回调)   │  │  - Shader 编译器     │  │
│  └─────────────────┘  │  - 状态映射           │  │
│                       │  - 格式转换           │  │
│                       └──────────┬───────────┘  │
│                                  │               │
│                       ┌──────────▼───────────┐  │
│                       │  Venus Encoder       │  │
│                       │  (Vulkan→二进制流)   │  │
│                       └──────────┬───────────┘  │
│                                  │               │
│                       ┌──────────▼───────────┐  │
│                       │  命令缓冲区打包       │  │
│                       │  (聚合一帧的命令)     │  │
│                       └──────────┬───────────┘  │
│                                  │               │
│  D3DKMTSubmitCommand ◄───────────┘               │
└────────────────────────┬─────────────────────────┘
                         │
┌────────────────────────▼─────────────────────────┐
│  KMD: vboxgpu_kmd.sys (内核态)                    │
│                                                    │
│  - 接收 UMD 提交的命令缓冲区                        │
│  - 通过 MMIO 写入虚拟 PCI 设备的 BAR               │
│  - 处理 VSync / Fence 完成中断                      │
│  - 管理虚拟显存分配                                 │
│  - 不包含任何翻译逻辑（纯搬运）                      │
└────────────────────────┬─────────────────────────┘
                         │
                    虚拟 PCI 设备
```

### 6.3 UMD 核心入口

```c
// vboxgpu_umd.dll 导出函数
// d3d11.dll 加载 UMD 时首先调用此函数

HRESULT APIENTRY OpenAdapter10_2(D3D10DDIARG_OPENADAPTER *pOpenData) {
    // 1. 填充设备创建回调
    pOpenData->pAdapterFuncs->pfnCalcPrivateDeviceSize = CalcPrivateDeviceSize;
    pOpenData->pAdapterFuncs->pfnCreateDevice = CreateDevice;
    pOpenData->pAdapterFuncs->pfnCloseAdapter = CloseAdapter;

    // 2. 报告支持的 DX 版本
    // D3D11_1DDI → DX11.1 Feature Level
    // 这决定了 d3d11.dll 会使用哪个版本的 DDI 回调表

    return S_OK;
}

// 创建设备时，返回完整的 DDI 函数指针表
HRESULT APIENTRY CreateDevice(
    D3D10DDI_HADAPTER hAdapter,
    D3D10DDIARG_CREATEDEVICE *pCreateData)
{
    // 初始化 DXVK 翻译引擎（运行在用户态，完全合法）
    auto* device = new RemoteDxvkDevice();
    device->initVenusEncoder();

    // 填充 200+ 个 DDI 回调
    D3D11_1DDI_DEVICEFUNCS *pFuncs = pCreateData->p11_1DeviceFuncs;

    pFuncs->pfnCreateResource          = UMD_CreateResource;
    pFuncs->pfnDestroyResource          = UMD_DestroyResource;
    pFuncs->pfnCreateVertexShader       = UMD_CreateVertexShader;
    pFuncs->pfnCreatePixelShader        = UMD_CreatePixelShader;
    pFuncs->pfnDraw                     = UMD_Draw;
    pFuncs->pfnDrawIndexed              = UMD_DrawIndexed;
    pFuncs->pfnPresent                  = UMD_Present;
    pFuncs->pfnSetBlendState            = UMD_SetBlendState;
    pFuncs->pfnSetDepthStencilState     = UMD_SetDepthStencilState;
    pFuncs->pfnSetRasterizerState       = UMD_SetRasterizerState;
    // ... 约 200 个回调

    return S_OK;
}

// 示例：Draw 回调实现
void APIENTRY UMD_Draw(
    D3D10DDI_HDEVICE hDevice,
    UINT VertexCount,
    UINT StartVertexLocation)
{
    auto* device = GetRemoteDevice(hDevice);

    // DXVK 翻译：将当前 DX11 渲染状态转换为 Vulkan 命令
    device->dxvkTranslate_Draw(VertexCount, StartVertexLocation);

    // Venus 编码：序列化 Vulkan 命令
    // （在 dxvkTranslate_Draw 内部完成，通过 RemoteVkDevice）

    // 命令流自动累积在缓冲区中，等 Flush/Present 时批量提交
}
```

### 6.4 KMD 核心职责

KMD 保持精简，只做以下事情：

1. **设备注册：** 向 dxgkrnl.sys 注册为 WDDM 1.1 Display Miniport Driver
2. **能力声明：** 通过 QueryAdapterInfo 告诉 Windows 这块 GPU 支持 DX11
3. **UMD 路径：** 通过 INF 文件指定 UMD DLL 的路径，dxgkrnl 会在每个 DX 进程中加载
4. **命令提交：** DxgkDdiSubmitCommand 接收 UMD 打包好的命令缓冲区，写入虚拟 PCI BAR
5. **中断处理：** 处理 Host 发来的 fence 完成和 VSync 中断
6. **显存管理：** 虚拟显存的分配和回收（实际由共享内存支撑）

### 6.5 驱动签名处理

| 环境 | 方案 |
|------|------|
| 开发/调试 | bcdedit /set testsigning on + 自签证书 |
| 个人使用 | 将自签根证书安装到系统证书库 |
| 公开发布 | 购买 EV 代码签名证书（$200-400/年）|
| 最终目标 | 集成到 VirtualBox Guest Additions 签名体系 |

### 6.6 阶段二交付物

在阶段一的基础上新增：

```
guest/
├── kmd/                          # 内核态驱动
│   ├── vboxgpu_kmd.c             # 主入口 + WDDM DDI
│   ├── hw_interface.c            # 虚拟 PCI BAR 读写
│   ├── memory_mgr.c              # 虚拟显存管理
│   ├── interrupt.c               # 中断处理
│   ├── vidpn.c                   # 显示输出配置
│   └── Makefile / sources        # WDK 构建文件
│
├── umd/                          # 用户态驱动
│   ├── entry.c                   # OpenAdapter10_2 入口
│   ├── device.cpp                # CreateDevice + DDI 表
│   ├── ddi_resource.cpp          # 资源相关 DDI 回调
│   ├── ddi_shader.cpp            # Shader 相关 DDI 回调
│   ├── ddi_draw.cpp              # 绘制相关 DDI 回调
│   ├── ddi_state.cpp             # 渲染状态 DDI 回调
│   ├── ddi_present.cpp           # Present/Flush DDI 回调
│   └── dxvk_integration.cpp      # DXVK Core 集成胶合层
│
└── inf/
    └── vboxgpu.inf               # 驱动安装信息
```

---

## 7. 阶段三：VirtualBox 深度集成

### 7.1 虚拟 PCI 设备

在 VirtualBox 源码中添加 `DevVBoxGpuBridge`，包含三个 BAR：

- **BAR0 (4KB MMIO)：** 控制寄存器（doorbell、fence、中断、分辨率配置）
- **BAR1 (16MB MMIO)：** 命令 Ring Buffer（Guest 写入，Host 消费）
- **BAR2 (512MB MMIO)：** 数据传输区（纹理上传、帧回传、Shader 字节码）

### 7.2 传输优化

从阶段一/二的 TCP/HGCM 切换到共享内存后，性能大幅提升：

- **命令传输：** Guest UMD 将一帧命令写入 BAR1（共享内存，不触发 VM Exit），帧末写一次 BAR0 doorbell 寄存器（1 次 VM Exit 触发 Host 处理）
- **纹理传输：** 直接写入 BAR2 的数据区，Host 直接读取（零拷贝）
- **帧回传：** Host 渲染完成后写入 BAR2 的帧缓冲区，KMD 将此区域设为显示源

### 7.3 GUI 集成

在 VirtualBox 的虚拟机设置中添加新的图形控制器选项：

```
显示 → 图形控制器:
  ☐ VBoxVGA
  ☐ VMSVGA
  ☐ VBoxSVGA
  ☑ VBoxGPUBridge (DX11 加速)    ← 新增选项
```

### 7.4 需要修改的 VirtualBox 源码

| 文件/目录 | 修改内容 |
|-----------|---------|
| src/VBox/Devices/Graphics/DevVBoxGpuBridge.cpp | 新增：虚拟 PCI 设备 |
| src/VBox/Devices/build/VBoxDD.cpp | 修改：注册新设备 |
| src/VBox/Main/src-server/MachineImpl.cpp | 修改：VM 配置 |
| src/VBox/Main/idl/VirtualBox.xidl | 修改：API 定义 |
| src/VBox/Frontends/VirtualBox/ | 修改：GUI 选项 |

---

## 8. 阶段四：性能优化与游戏兼容

### 8.1 性能优化清单

| 优化项 | 预期收益 | 难度 |
|--------|---------|------|
| 命令批量提交（每帧 1 次 doorbell） | 减少 VM Exit，提升 10-50% | 中 |
| Shader 编译缓存（磁盘持久化） | 消除首次加载卡顿 | 低 |
| Pipeline 状态缓存 | 减少 Pipeline 创建开销 | 低（DXVK 已有） |
| 纹理压缩传输（保持 BC 格式） | 减少 75% 纹理带宽 | 低 |
| 帧回传硬件编码（NVENC/VCE） | 降低回传延迟和 CPU 占用 | 高 |
| 多线程命令录制 | 利用多核 CPU | 高 |
| 内存映射优化（脏页追踪） | 减少不必要的数据传输 | 高 |

### 8.2 游戏兼容性路线

按 DX11 功能复杂度逐步覆盖：

```
阶段     支持的功能                          代表游戏
─────────────────────────────────────────────────────────
Level 1  基础渲染（三角形、纹理、基础 Shader） DX SDK Samples
Level 2  完整渲染状态（混合、深度、光栅化）    Minecraft DX11
Level 3  复杂 Shader + MRT + 后处理            Portal 2
Level 4  Compute Shader + 高级纹理格式         Skyrim SE
Level 5  完整 DX11.1 + 反锯齿                 Witcher 3
```

---

## 9. 可复用的开源资源清单

### 9.1 直接复用（代码级别）

| 项目 | 复用内容 | 许可证 | 地址 |
|------|---------|--------|------|
| DXVK | Shader 编译器, 状态映射, 格式转换 | zlib | github.com/doitsujin/dxvk |
| Venus Protocol | 命令序列化协议定义 | MIT | gitlab.freedesktop.org/virgl/venus-protocol |
| virglrenderer | Venus 解码器 + Host 端 Vulkan 执行 | MIT | gitlab.freedesktop.org/virgl/virglrenderer |
| Gfxstream | Vulkan 编解码代码生成器, Ring Buffer | Apache 2.0 | github.com/google/gfxstream |

### 9.2 参考借鉴（架构/模式）

| 项目 | 参考内容 | 地址 |
|------|---------|------|
| virtio-win (viogpudo) | WDDM KMD 驱动框架 | github.com/virtio-win/kvm-guest-drivers-windows |
| UTM virtio-gpu-wddm-dod | 精简的 WDDM DOD 驱动 | github.com/utmapp/virtio-gpu-wddm-dod |
| VirtualBox VMSVGA 源码 | 虚拟 PCI 设备, HGSMI, 显示驱动 | virtualbox.org 源码树 |
| Looking Glass | 帧传输共享内存设计 | looking-glass.io |
| Mesa SVGA3D 驱动 | VMware 虚拟 GPU Guest 驱动 | docs.mesa3d.org/drivers/svga3d.html |
| Keenuts/virtio-gpu-win-icd | Windows OpenGL ICD PoC | github.com/Keenuts/virtio-gpu-win-icd |
| virtio-win PR #943 | Windows 3D 加速最新进展 | github.com/virtio-win/kvm-guest-drivers-windows/pull/943 |

### 9.3 许可证兼容性

| 我们的组件 | 依赖 | 依赖许可证 | 兼容性 |
|-----------|------|-----------|--------|
| Guest DXVK 魔改 | DXVK | zlib | 可以任意使用，包括闭源 |
| Guest Venus 编码 | venus-protocol | MIT | 完全自由 |
| Host Venus 解码 | virglrenderer | MIT | 完全自由 |
| VBox 设备 | VirtualBox | GPLv3 | 需要开源（符合我们目标） |

**结论：** 所有依赖的许可证都与 GPLv3 开源发布兼容。

---

## 10. 关键技术风险与应对

### 10.1 高风险

| 风险 | 影响 | 概率 | 应对 |
|------|------|------|------|
| DXVK 魔改为"录制模式"难度超预期 | 阶段一延期 | 中 | 可退回到自研最小 Vulkan 编码器，不依赖 DXVK |
| Venus 编码器 Windows 移植问题 | 阶段一延期 | 低 | 核心编解码是纯 C，平台依赖少 |
| 同步操作延迟过高 | 帧率很低 | 中 | 异步化：尽量延迟同步点，批量回读 |
| Win7 上 Vulkan ICD 加载问题 | 阶段一阻塞 | 低 | 阶段一不需要 ICD，DXVK 直接输出命令流 |

### 10.2 中风险

| 风险 | 影响 | 概率 | 应对 |
|------|------|------|------|
| VirtualBox 源码编译困难 | 阶段三延期 | 中 | 阶段三才需要，有充足准备时间 |
| WDDM 驱动导致蓝屏 | 阶段二调试困难 | 中 | 从 KMDOD 示例出发，最小化内核代码 |
| 纹理传输带宽不足 | 游戏卡顿 | 中 | BC 压缩格式传输 + 增量更新 |
| Shader 编译导致卡顿 | 首次加载慢 | 低 | 磁盘缓存（DXVK 已有机制） |

### 10.3 低风险

| 风险 | 影响 | 概率 | 应对 |
|------|------|------|------|
| 驱动签名问题 | 阶段二部署不便 | 低 | 测试签名模式（开发阶段足够） |
| 游戏反作弊检测 DLL 代理 | 阶段一部分游戏不可用 | 低 | 阶段一仅验证，阶段二驱动层完全透明 |

---

## 11. 项目结构与仓库规划

### 11.1 仓库结构

```
vbox-gpu-bridge/
│
├── README.md                      # 项目介绍
├── LICENSE                        # GPLv3
├── ROADMAP.md                     # 路线图与进度
│
├── guest/
│   ├── dxvk-remote/               # DXVK 魔改版本（Git submodule + patches）
│   │   ├── patches/               # 针对上游 DXVK 的 patch 集
│   │   └── src/
│   │       └── dxvk/
│   │           ├── dxvk_remote_device.h/cpp
│   │           └── dxvk_remote_cmdlist.h/cpp
│   │
│   ├── venus-encoder/             # Venus 编码器 Windows 移植
│   │   ├── CMakeLists.txt
│   │   ├── vn_encode_core.c
│   │   ├── vn_encode_pipeline.c
│   │   ├── vn_encode_cmd.c
│   │   └── vn_transport_win.c
│   │
│   ├── kmd/                       # 内核态驱动（阶段二）
│   │   ├── vboxgpu_kmd.c
│   │   ├── hw_interface.c
│   │   ├── memory_mgr.c
│   │   ├── interrupt.c
│   │   └── vidpn.c
│   │
│   ├── umd/                       # 用户态驱动（阶段二）
│   │   ├── entry.c
│   │   ├── device.cpp
│   │   ├── ddi_*.cpp              # DDI 回调实现
│   │   └── dxvk_integration.cpp
│   │
│   ├── transport/                 # Guest 侧传输层
│   │   ├── transport_interface.h  # 抽象接口
│   │   ├── transport_tcp.cpp      # TCP 实现
│   │   ├── transport_hgcm.cpp     # HGCM 实现
│   │   └── transport_pci.cpp      # PCI BAR 实现（阶段三）
│   │
│   └── inf/
│       └── vboxgpu.inf
│
├── host/
│   ├── venus-decoder/             # Venus 解码器 Windows 移植
│   │   ├── CMakeLists.txt
│   │   ├── vn_decode_core.c
│   │   ├── vn_decode_pipeline.c
│   │   ├── vn_decode_cmd.c
│   │   └── vn_dispatch.c
│   │
│   ├── vulkan-backend/            # Vulkan 渲染执行
│   │   ├── vk_instance.cpp
│   │   ├── vk_resource_map.cpp
│   │   ├── vk_executor.cpp
│   │   └── vk_window.cpp
│   │
│   ├── transport/                 # Host 侧传输层
│   │   ├── transport_tcp.cpp
│   │   ├── transport_hgcm.cpp
│   │   └── transport_pci.cpp
│   │
│   └── frame-delivery/
│       ├── host_window.cpp        # 阶段一：Host 窗口显示
│       └── shared_memory.cpp      # 阶段二+：帧回传
│
├── vbox-device/                   # VirtualBox 虚拟 PCI 设备（阶段三）
│   ├── DevVBoxGpuBridge.cpp
│   ├── DevVBoxGpuBridge.h
│   └── vbox_patches/             # VBox 源码 patch 集
│
├── common/
│   ├── protocol/                  # Guest/Host 共享的协议定义
│   │   ├── venus_protocol.h       # Venus 协议（来自上游）
│   │   ├── transport_protocol.h   # 传输层协议
│   │   └── frame_protocol.h       # 帧交付协议
│   │
│   └── utils/
│       ├── ring_buffer.h          # 无锁 Ring Buffer
│       └── resource_id.h          # 资源 ID 生成
│
├── tools/
│   ├── cmd_recorder/              # 命令流录制工具
│   ├── cmd_player/                # 命令流回放工具（脱离 VM 调试）
│   └── perf_monitor/              # 性能监控
│
├── tests/
│   ├── dx11_triangle/             # 最小 DX11 测试
│   ├── dx11_textured_quad/        # 纹理测试
│   ├── dx11_cube/                 # 3D 变换测试
│   ├── venus_roundtrip/           # Venus 编解码正确性测试
│   ├── transport_benchmark/       # 传输带宽测试
│   └── game_compat/               # 游戏兼容性测试脚本
│
└── docs/
    ├── DESIGN.md                  # 本文档
    ├── BUILD.md                   # 构建指南
    ├── DXVK_MOD.md                # DXVK 魔改详解
    ├── VENUS_PORT.md              # Venus 移植详解
    ├── WDDM_DRIVER.md             # WDDM 驱动详解
    ├── VBOX_DEVICE.md             # VBox 虚拟设备详解
    └── COMPAT.md                  # 游戏兼容性列表
```

### 11.2 构建系统

| 组件 | 构建系统 | 说明 |
|------|---------|------|
| dxvk-remote | Meson (cross-compile) | 沿用 DXVK 原有构建 |
| venus-encoder/decoder | CMake | 跨平台 C 代码 |
| Guest KMD | WDK (build/nmake) | Windows 内核驱动标准构建 |
| Guest UMD | CMake + MSVC | 用户态 DLL |
| Host 渲染后端 | CMake + MSVC | 用户态程序 |
| VBox 设备 | kBuild | VirtualBox 构建系统 |

---

## 12. 调试策略

### 12.1 命令流录制/回放（最重要的调试工具）

在传输层插入录制钩子，将 Guest→Host 的完整命令流 dump 到文件：

```
Guest 运行游戏 → 命令流录制到 capture_001.bin
                      │
Host 命令回放工具 ◄────┘ （不需要 VM，独立运行）
      │
      ▼
Vulkan 渲染 → 窗口显示（可逐帧/逐命令单步）
```

这样可以在完全不启动虚拟机的情况下调试 Host 渲染引擎，极大加速开发迭代。

### 12.2 各组件调试方法

| 组件 | 调试工具 | 说明 |
|------|---------|------|
| DXVK 翻译层 | Visual Studio + DXVK_LOG | 附加到游戏进程 |
| Venus 编码/解码 | 命令流 dump + 对比工具 | 验证编解码一致性 |
| 传输层 | Wireshark(TCP) / 日志 | 检查数据完整性 |
| Host Vulkan 后端 | RenderDoc + Validation Layers | 截帧分析 |
| KMD | WinDbg 内核调试 | 通过 VBox 串口 |
| UMD | Visual Studio 附加进程 | 用户态调试 |
| VBox 虚拟设备 | VBox 日志 (LogRel) | VBox 内置日志 |

### 12.3 诊断 HUD

在 Host 渲染窗口上叠加诊断信息（类似 DXVK 的 HUD）：

```
VBox GPU Bridge v0.1
────────────────────
FPS: 45
Commands/frame: 3,247
Textures uploaded: 12 (4.2 MB)
Shader compiles: 0 (cache: 127)
Transport latency: 0.3 ms
Pending fences: 2
VRAM usage: 128 / 512 MB
```

---

## 13. 开源与社区策略

### 13.1 许可证

GPLv3（与 VirtualBox 一致），确保与 VBox 源码的兼容性。

DXVK (zlib) 和 Venus (MIT) 的代码可以在 GPLv3 项目中使用。

### 13.2 项目命名

建议名称：**vbox-gpu-bridge**

含义：VirtualBox GPU Bridge，连接 Guest DX11 和 Host Vulkan 的桥梁。

### 13.3 社区协作方向

- **virtio-win 社区：** 他们正在做的 Windows 3D 驱动（PR #943）与我们的目标有重叠。长期可以协作，共享 WDDM 驱动的经验和代码。
- **DXVK 社区：** 我们的"录制模式"魔改如果做得好，可以作为 DXVK 的一个可选后端提交 upstream。
- **Venus/virglrenderer 社区：** Windows 移植的工作可以回馈给社区。
- **VirtualBox 社区：** 如果项目成功，可以提议作为 VirtualBox 的官方扩展。

### 13.4 长期演进方向

如果 virtio-win 的 Windows Venus 驱动（通过 VirtIO-GPU 传输 Vulkan）最终成熟，我们的方案可以演进为：

**在 VirtualBox 中实现标准的 virtio-gpu 设备** → 直接复用社区的 Windows Guest 驱动 → 从"独立项目"变成"VirtualBox 的 virtio-gpu 后端"

这样维护成本大幅降低，同时能持续受益于整个社区的进步。

---

## 附录 A：工作量估算总览

| 阶段 | 组件 | 原创代码量 | 复用代码量 | 工期 |
|------|------|-----------|-----------|------|
| 一 | Host Venus 解码+Vulkan | ~3,000 行 | ~15,000 行 | 3 周 |
| 一 | DXVK 魔改 | ~2,000 行 | ~80,000 行 | 6 周 |
| 一 | Venus 编码器移植 | ~1,000 行 | ~10,000 行 | (含上) |
| 一 | 传输层(TCP/HGCM) | ~1,500 行 | - | 4 周 |
| **一合计** | | **~7,500 行** | **~105,000 行** | **~13 周** |
| 二 | WDDM UMD | ~5,000 行 | (复用阶段一) | 8 周 |
| 二 | WDDM KMD | ~3,000 行 | - | 6 周 |
| 二 | 帧回传 | ~1,500 行 | - | (含上) |
| **二合计** | | **~9,500 行** | | **~14 周** |
| 三 | VBox 虚拟 PCI 设备 | ~4,000 行 | - | 6 周 |
| 三 | 共享内存传输 | ~1,000 行 | - | 4 周 |
| 三 | GUI 集成 | ~300 行 | - | 2 周 |
| **三合计** | | **~5,300 行** | | **~12 周** |
| | | | | |
| **项目总计** | | **~22,300 行原创** | **~105,000 行复用** | **~39 周** |

原创代码与复用代码的比例约为 1:5，这正是大量复用 DXVK + Venus 的价值所在。

---

## 附录 B：首日开发环境搭建清单

### Host 开发环境

- Windows 10/11 (22H2+)
- Visual Studio 2022 (C++ 桌面开发 + MSVC v143)
- Vulkan SDK (latest, vulkan.lunarg.com)
- CMake 3.25+
- Git

### Guest 测试环境

- VirtualBox 7.x (latest)
- Windows 7 SP1 x64 虚拟机
- VirtualBox Guest Additions
- DirectX SDK (June 2010) 或 Windows SDK
- DX11 测试程序

### 阶段二额外需要

- WDK 7.1 (Win7 WDDM 1.1 驱动编译)
- WinDbg (内核调试)

### 阶段三额外需要

- VirtualBox 源码 + kBuild 构建环境
- Qt 5.x (VBox GUI)
