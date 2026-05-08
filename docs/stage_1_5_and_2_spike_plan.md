# 阶段 1.5 产品化 + 阶段二侦察并行规划

最后更新：2026-05-07

## 现状判断

阶段一 M1.6+ 架构验证完成：
- DX11 → DXVK → ICD → Venus → TCP → Host Vulkan 全链路在 SortTheCourt / UltraKill / Heaven 三款真实游戏跑通
- 协议、渲染特性（Tess/Geom/Compute/Cubemap/MSAA/MRT）、长稳定性、多 client 已就位
- FPS 退化根因（ICD 热路径 fprintf）已修复
- 资源累积泄漏已清理

**直接跳阶段二（WDDM 驱动）不解决用户痛点**（FPS、画质），且工程风险大。  
推荐：阶段 1.5 产品化 + 阶段二侦察 spike **并行**进行。

---

## Track A：阶段 1.5 产品化

目标：把架构打磨到非开发者可用，作为"可发布预览版"。

### A1. ICD 健壮性（约 0.5 周）

> **范围澄清**：不尝试在 host 崩溃后保住游戏继续运行。绝大多数 D3D11
> 游戏不实现 device-lost 恢复路径，会崩或卡死，这是上层应用的问题。
> ICD 这一层目标是：host 死了之后 ICD 自身**不僵尸 / 不卡死**，让
> guest 进程可以被正常清理或外部 kill。**dev/test 价值为主**。

| 任务 | 描述 | 估时 |
|------|------|------|
| host 断连优雅降级 | recv 失败时进入 DEVICE_LOST 状态，关闭 socket，清理 pending queue，唤醒所有等待 CV 的线程 | 0.5 d |
| device-lost 传播 | 任意 vk 入口检查 `deviceLost_` 标志后立即返回 `VK_ERROR_DEVICE_LOST`，让 DXVK 转译为 DXGI_ERROR_DEVICE_REMOVED | 0.25 d |
| 超时控制 | acquire / fence wait / sync BDA 等长超时改有限超时（例如 5s），超时按 device-lost 处理 | 0.25 d |
| 自动化测试 hook | 单元测试：模拟 host 中途断开，guest ICD 干净退出 | 0.25 d |

### A1.5 动态生命周期场景（**优先**，约 1-1.5 周）

> **优先关注的产品化场景**：玩家在游戏内反复切分辨率 / 全屏窗口切换 /
> 游戏退出关卡重建 D3D11 device。当前实现走通了基本路径但有资源泄漏
> 和 race，反复切换累积后 driver 资源耗尽。这块是产品体验的核心，
> 优先级高于 A4 兼容性收尾。

#### A1.5a 分辨率切换闭环

**当前状态**：
- ICD 端 `icd_vkDestroySwapchainKHR` 是空实现 → host 不收销毁通知
- 旧 swapchain / images / imageViews 在 host 端永远不释放
- 旧 sentinel image (`0xFFF00000+i`) 被新的覆盖到 `images_` map，旧 VkImage handle 泄漏但 driver 内部仍引用
- mid-flight race：CreateSwapchain 瞬间，仍有 in-flight batch 引用旧 sentinel

| 任务 | 描述 | 估时 |
|------|------|------|
| 协议补完 | 新增 `VN_CMD_BRIDGE_DestroySwapchain` 命令 | 0.5 d |
| ICD 端 swapchain ID 跟踪 | DXVK→ICD 的 vkDestroySwapchainKHR 拿不到 ID（当前空实现），需要在 CreateSwapchain 时记录 ID 表 | 0.5 d |
| Host 端销毁路径 | `vkDeviceWaitIdle` + 销毁旧 imageViews + 旧 swapchain images 从 `images_` 移除 + `vkDestroySwapchainKHR` + `swapchains_.erase` | 1 d |
| mid-flight 同步 | CreateSwapchain 前 ICD 发同步 barrier 命令等 host 处理完 in-flight batch | 1 d |
| 反复切换测试程序 | dx11 测试：1 秒一次循环切 800x600 / 1920x1080 / 1280x720，跑 5 分钟无累积 | 1 d |
| 全屏 / 窗口切换 | 验证 surface caps 重新查询 + present mode 重新协商 | 0.5 d |

#### A1.5b D3D11 Device 重建闭环

**当前状态**：
- `video_restart` 等场景下游戏 ReleaseDevice 后再 D3D11CreateDevice 会失败
- ICD 端 `icd_vkDestroyDevice` 已有 ResetDescriptorPool 的 hack，但不完整
- DXUT 模式：device 重建很常见

| 任务 | 描述 | 估时 |
|------|------|------|
| ICD vkDestroyDevice 全清理 | 不依赖 host VkDevice 销毁的副作用，显式发 cmdBridgeResetSession 让 host 重建 decoder 状态（保留 TCP 连接） | 1.5 d |
| Host session reset | 收到 reset 命令后：vkDeviceWaitIdle，destroy 当前 VkDevice + swapchain + 所有资源，重新 createLogicalDevice，准备接受新 CreateDevice | 1 d |
| ICD 端状态归零 | bdaCache / mappedRegions / memoryShadows / descriptorPoolIds_ / descSetLastSeen_ 全部 clear | 0.5 d |
| 测试用例 | 反复 D3D11CreateDevice / Release device，10 次循环不泄漏 | 0.5 d |
| video_restart 场景 | UltraKill / DXUT 应用的实际 device 重建跑通 | 1 d |

#### A1.5c 验收门槛

- ✅ 反复切分辨率 30 次后 host VRAM 占用稳定（不增长）
- ✅ ICD-DIAG 显示 swapchains/imageViews/images 计数稳定
- ✅ 反复 device 重建 10 次后 host 内部 map 计数回到初始值
- ✅ 切换瞬间无可见花屏 / 闪烁

### A2. 日志系统重写（约 0.5 周）

替换分散的 `fprintf(stderr, ...)` 为统一 logger：

- 级别：ERROR / WARN / INFO / DEBUG / TRACE
- 输出目标：环形缓冲（默认）/ 文件 / 控制台，按环境变量切换
- **节流**：同消息 N 秒内最多 K 条
- 默认 ERROR + WARN，热路径零开销
- DIAG / RT_LOG 走独立通道（不和热路径竞争锁）

### A3. 安装与部署（约 1 周）

| 任务 | 描述 | 估时 |
|------|------|------|
| 安装器 | NSIS / Inno 脚本，自动复制 DXVK + ICD + 注册 ICD JSON | 2 d |
| ICD 注册 | 写注册表 `HKLM\Software\Khronos\Vulkan\Drivers` 注册 ICD（避免 test_env 模式） | 0.5 d |
| 配置文件 | `vbox_gpu.ini`：端口 / 日志级别 / MAX_IN_FLIGHT / 渲染窗口位置 | 1 d |
| 卸载器 | 干净卸载，恢复原始 D3D11 | 0.5 d |
| 启动脚本 | host server 加 Windows 服务模式（可选） | 1 d |

### A4. 兼容性收尾（约 1-1.5 周）

| 任务 | 描述 | 估时 |
|------|------|------|
| UltraKill FPS | 当前 27-71 不稳，目标 60 稳 | 3 d |
| Heaven 金属绿 | cubemap 反射采样错误 | 2 d |
| Heaven 地面 | tessellation displacement 缺失 | 3 d |

### A5. 测试与文档（约 0.5 周）

- 自动化烟雾测试：录制 → replay → 帧对比，3 款游戏各 1 个场景
- 用户安装文档 + 已知限制清单
- 开发者快速上手文档

### Track A 时间估算

| 子项 | 估时 |
|------|------|
| A1 host 断连优雅降级 | 0.5 周 |
| A1.5 动态生命周期（分辨率/device 重建）**优先** | 1-1.5 周 |
| A2 日志系统 | 0.5 周 |
| A3 安装与部署 | 1 周 |
| A4 兼容性收尾 | 1-1.5 周 |
| A5 测试与文档 | 0.5 周 |

**总计 4.5-5.5 周**。A1.5 是核心产品化任务（动态切分辨率 / device 重建），不能跳过。

---

## Track B：阶段二侦察 Spike

目标：评估 WDDM 驱动开发的可行性、环境搭建周期、技术风险。**不是全力实现**。

### B1. 环境搭建（约 1 周）

| 任务 | 描述 |
|------|------|
| WDK 7.1 安装 | Win7 时代驱动开发包 |
| Win7 SP1 64-bit VM | 单独的 VM 实例（与日常测试 VM 分开） |
| 内核调试通道 | VBox `--uart` 配 named pipe → 宿主 WinDbg 连接 |
| testsigning 模式 | `bcdedit /set testsigning on` |
| 开发期签名 | 自签证书 + cert install |
| WinDbg 接管 | 启动期断点验证内核调试可用 |

### B2. 最小骨架 KMD（约 1-2 周）

目标：能加载、被 PnP 识别为显卡、不蓝屏。**不渲染**。

```
vboxgpu_kmd.sys
├─ DriverEntry
├─ DxgkDdiAddDevice   返回 PDEVICE_OBJECT
├─ DxgkDdiStartDevice 检测虚拟设备（暂时假设 PCI 存在）
├─ DxgkDdiQueryAdapterInfo 报告"虚拟 GPU"硬件能力
└─ DxgkDdiStopDevice / RemoveDevice  PnP 清理
```

成功标准：Device Manager 看到"VBox GPU Bridge"设备，无黄色感叹号。

### B3. 最小骨架 UMD（约 1 周）

目标：让 Win7 D3D 子系统能在这块"显卡"上创建 D3D11 设备。**仍不真渲染**。

```
vboxgpu_umd.dll
├─ OpenAdapter            UMD 入口
├─ CreateDevice           创建 D3D 设备上下文
├─ GetCaps                返回最小可用的 D3D11 能力
└─ Destroy*               清理
```

成功标准：在 guest 跑 `dx11_triangle` 选这块显卡，能进入 `D3D11CreateDevice` 不崩。**画面不对没关系**——画面可能黑屏，关键是创建链路通了。

### B4. ICD encoder 接驳点（约 1 周）

把现有 ICD 的 encoder 抽出独立 lib（`vbox_venus_encoder.lib`），UMD 链接它：

```
现状: vbox_vulkan.dll = 全部 ICD 逻辑 + Venus encoder
拆分: vbox_venus_encoder.lib （独立）
       ├─ 被 vbox_vulkan.dll （阶段一 ICD）链接
       └─ 被 vbox_vulkan_umd.dll （阶段二 UMD 实验）链接
```

UMD 的 `RenderCB` 入口收 D3D 命令 → 翻译 → 走 Venus encoder。

成功标准：dx11_triangle 在 stage 2 模式下能渲染出三角形（可以歪、可以错色，**但 host 端能收到 batch**）。

### B5. 决策点

Spike 完成（B1-B4 全成）后：

- **顺利**：技术风险已知，工作量可估，**进入阶段二全力推进**
- **碰大坑**：写报告分析卡点（驱动签名 / Win7 兼容 / WDK 限制），评估是否：
  - 换路线（Win10 + DCH driver / WDDM 1.3 hack）
  - 推迟阶段二，继续阶段 1.5+ 优化
  - 接受现状，宣布项目以阶段 1.5 收官

### Track B 时间估算

**4-6 周** spike，结束后才决定全推 or 中止。

---

## 并行策略

### 解耦保证

| 维度 | A 改 | B 改 |
|------|------|------|
| `vn_command.h` | ❌ 不改 | ❌ 不改（B 复用 A 现有协议）|
| `vn_encoder.h` | 🟡 仅在 wire-format 不变前提下改 | ❌ 不改 |
| ICD 内部 | ✅ 自由改 | ❌ 不动 |
| host decoder | ✅ 自由改 | ❌ 不动 |
| 新建 KMD/UMD 子目录 | ❌ 不动 | ✅ 自由 |

### 协议冻结期

Track B 完成 B4 接驳后，**协议进入冻结**：
- A 仍可改 ICD 内部、host decoder 内部、logger、安装等
- A 不能改：cmd type ID、参数 layout、batch 框架
- 必要的协议修改要发联合 commit，两边同步

### 共用资源排期

- 物理 GPU：交替使用（一次只跑一个 host server 实例）
- VBox VM：A 用普通 Win7 VM，B 用 kernel-debug Win7 VM（独立实例）
- WinDbg：B 专用，A 用 Visual Studio 用户态调试

### 单人节奏建议

- **状态好的整块时间**做 B（驱动开发要专注，cycle 长）
- **碎片时间 / 杂事多的日**做 A（短周期任务可中断）
- 周一/周三 专 B，其余 A，避免上下文切换疲劳

### 起步顺序

第 1 周并行：
- A：A1 host 断连优雅降级 + A2 logger 雏形
- B：B1 环境搭建

第 2-3 周：
- A：**A1.5 动态生命周期场景**（优先核心）
- B：B2 骨架 KMD

第 4 周：
- A：A3 安装与部署
- B：B3 骨架 UMD

第 5-6 周：
- A：A4 兼容性收尾 + A5 测试文档
- B：B4 encoder 接驳点

第 6-7 周：决策点 + Track A 收尾。

---

## 风险登记

| 风险 | 概率 | 影响 | 缓解 |
|------|------|------|------|
| WDK 7.1 在新 Windows 主机上无法安装 | 中 | 高 | 早期 spike 验证 |
| Win7 内核调试通道不稳 | 低 | 中 | 备用通道（1394 / 网络） |
| 驱动签名问题影响游戏运行 | 低 | 高 | testsigning 开发期足够，发布期评估 EV cert |
| Track A 协议改动破坏 B 集成 | 中 | 中 | 协议冻结策略 + 联合 commit 约定。A1.5 新增 BridgeDestroySwapchain / ResetSession 命令在 B4 接驳前完成 |
| 动态切分辨率 mid-flight race 难根除 | 中 | 中 | A1.5 引入显式同步 barrier 命令，必要时退化为完全序列化（损失少量并发性能换稳定） |
| device 重建后 host VkDevice 状态泄漏 | 中 | 高 | A1.5b 显式 ResetSession 协议 + 验收门槛要求 10 次循环 map 计数回归 |
| UltraKill 60 FPS 目标做不到 | 中 | 低 | 60 FPS 是目标不是 blocker，留 30 FPS 兜底 |
| Heaven 渲染瑕疵根因深 | 高 | 中 | 时限（每个 bug 5 天上限），超时降级为已知问题 |

---

## 阶段二全力推进时（spike 通过后）

留作后续规划，不在本文档详述。大致方向：

- 阶段 2.1：UMD 完整 D3D11 DDI 覆盖（~1-2 月）
- 阶段 2.2：KMD 命令搬运 + DMA buffer（~1 月）
- 阶段 2.3：原版 d3d11.dll 兼容性测试（~2-4 周）
- 阶段 2.4：Driver signing / 发布流程（~2 周）

总计 3-6 月，与原 CLAUDE.md 估计一致。

---

## 决策矩阵

| 完成情况 | 状态 | 下一步 |
|---------|------|--------|
| Track A 完成 + Track B spike 通过 | 🟢 理想 | 全力阶段二 |
| Track A 完成 + Track B 卡顿 | 🟡 可接受 | 阶段一收官发布，二期重新评估 |
| Track A 卡顿 + Track B 通过 | 🟡 不太可能 | 优先 A 兜底再推 B |
| 两条都卡 | 🔴 警报 | 项目重新评估 |

---

## 进度追踪

每周更新本文档对应章节状态（待开始 / 进行中 / 完成 / 阻塞）。Track A/B 各自有独立 commit 前缀：

- `stage1.5-A1: ...` 等
- `stage2-spike-B1: ...` 等

便于后期 git log 区分两条 track 的进度。
