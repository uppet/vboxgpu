# 从静态三角形到变色动画三角形 — 踩坑记录

## 背景

M1.2 里程碑达成了 DX11 三角形端到端渲染：Guest DX11 -> DXVK -> ICD -> TCP -> Host Vulkan。
三角形显示固定红色，证明管线通了。

下一步：让三角形颜色随时间变化（pixel shader 用 cbuffer 传 time 值做 HSV 色环），
验证 buffer 数据传输和 uniform buffer descriptor 绑定是否正确。

## 核心问题：三角形始终红色不变色

### 现象
dx11_triangle 改为变色 shader 后，Host 窗口始终显示固定红色三角形。
Host 内置截图（captureScreenshot）确认 frame 5 和 frame 150 像素都是 RGB(255,0,0)。

### 最终根因
`handleCreateDescriptorSetLayout` 给所有 layout 设了 `VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR`。
Vulkan 规范明确：**带此标志的 layout 不能用于 `vkAllocateDescriptorSets`**。
DXVK 用的是传统 descriptor set 路径（Allocate + Update + Bind），不是 push descriptor。
导致 allocate 出的 descriptor set 无效，GPU 从中读到全零，shader 的 time=0 → 固定红色。

### 为什么之前没发现
M1.2 的静态三角形 shader 是固定颜色 `return float4(1,0,0,1)`，不读任何 descriptor/uniform buffer。
即使 descriptor set 全部无效，固定色 shader 也能正确渲染。只有引入 cbuffer（需要 GPU 从 descriptor 读 buffer 数据）后问题才暴露。

### 历史溯源
- **commit a8262bf**：发现 DXVK blit pass 用 push descriptor，给 layout 加了 PUSH_DESCRIPTOR_BIT
- **commit 2016ff2**：禁用 descriptor_buffer 后 DXVK 退回传统 descriptor set 路径。但 PUSH_DESCRIPTOR_BIT **没有移除**
- 从那时起所有 descriptor set layout 都带着错误的 flag，但静态三角形不读 descriptor，看不出问题

### 修复
一行代码：`info.flags = 0;` 替换掉 `info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;`

---

## 过程中的其他坑和错误探索

### 坑1：Shadow memory 独立 calloc（已修复，但非动画不工作的根因）

**问题**：ICD 的 `vkMapMemory` 每次调用都 `calloc` 独立 buffer。DXVK 对同一个 VkDeviceMemory 只调一次 `vkMapMemory` 获取 persistent mapped pointer，后续通过 offset 访问不同 slice。如果 size 参数是 `VK_WHOLE_SIZE`（~0ULL），calloc 会失败返回 NULL。

**修复**：在 `AllocateMemory` 时按 allocationSize 分配完整 shadow buffer，`MapMemory` 返回 `shadow_base + offset`。

**教训**：DXVK 重度依赖 persistent mapping，ICD 必须维护完整的 per-memory shadow buffer。

### 坑2：误判 push constants 是 CB 数据载体

**现象**：ICD 日志显示 `vkCmdPushConstants` 数据全零（48 bytes, FRAGMENT stage）。
一开始以为 push constants 是 D3D11 constant buffer 的数据载体，花了大量时间追踪为什么全零。

**真相**：DXVK 的 push constants 传的是内部元数据（binding offsets 等），不是用户的 constant buffer 数据。
实际的 CB 数据通过 `vkUpdateDescriptorSets`（uniform buffer descriptor）+ `vkCmdBindDescriptorSets` 传递。

**教训**：不要假设 Vulkan API 调用的用途，要通过 descriptor type 和 binding 来追踪数据流。

### 坑3：ICD DLL 的 stderr 不可见

**问题**：ICD DLL（vbox_vulkan.dll）使用静态 CRT 链接。其 `fprintf(stderr, ...)` 不会出现在宿主进程的 stderr pipe 中。delegate-runner 捕获不到 ICD 的调试输出。

**症状**：连续多次测试，ICD 的日志完全消失。只有 dx11_triangle 主程序的 fprintf(stderr) 能看到。

**修复**：改用 Win32 API `CreateFileA/WriteFile` 直接写文件 `icd_trace.log`。

**教训**：Windows DLL 静态 CRT 有独立的 FILE* 表，stderr 不共享。DLL 调试必须用文件或 OutputDebugString。

### 坑4：从错误的目录运行 dx11_triangle.exe

**问题**：命令用 `S:/bld/vboxgpu/build/tests/dx11_triangle/Debug/dx11_triangle.exe`（build 输出路径）运行。
Windows DLL 搜索先找 exe 所在目录 → 找到系统 d3d11.dll → DXVK 不加载 → ICD 完全不参与。

**症状**：测试跑起来了、帧数正常，但 ICD 日志文件不存在、DXVK 日志没更新。浪费了多轮排查。

**修复**：必须从 `test_env` 目录运行（`./dx11_triangle.exe`），该目录有 DXVK 的 d3d11.dll/dxgi.dll。

**教训**：Windows DLL 加载顺序：exe 目录 > 系统目录 > 工作目录。DXVK DLL 必须在 exe 同目录。

### 坑5：用 WGC 截图脚本验证渲染结果

**问题**：capture_window.py 用 WGC 从外部截窗口，经常抓错窗口（抓到 guest 窗口而非 host 窗口），
采样数据与实际渲染不一致（可能截到 DWM 合成后的内容或过渡帧）。

**症状**：截图显示颜色在变化，以为动画生效了。实际用户肉眼看窗口是固定红色。

**修复**：使用 host server 内置的 `captureScreenshot()` 函数，直接从 Vulkan swapchain image 读取像素写 BMP。

**教训**：渲染结果验证必须从 GPU 侧读回（readback），不能依赖窗口系统截图。

### 坑6：ClearColorImage 转发导致全白

**问题**：实现 `vkCmdClearColorImage` 转发后，host 窗口全白。
因为 guest 发送的 image layout（TRANSFER_DST_OPTIMAL）与 host 端 image 的实际 layout 不匹配，
且没有做 layout transition，clear 操作导致后续渲染全部异常。

**修复**：暂时回退为 no-op，改在 host BeginRendering 中对 swapchain pass 强制 `LOAD_OP_CLEAR`。

**教训**：`vkCmdClearColorImage` 需要正确的 image layout，不能简单转发 guest 的 layout 参数。
host 端的 image 管理（layout tracking）是必要的基础设施。

### 坑7：host stderr 缓冲导致日志丢失

**问题**：host server 的 `fprintf(stderr, ...)` 在重定向到文件时变成全缓冲，进程被 kill 后数据未 flush。

**修复**：main() 中添加 `setvbuf(stderr, NULL, _IONBF, 0)` 强制无缓冲。

**教训**：Windows 下 stderr 重定向到文件时不再是 unbuffered，需要显式设置。

---

## 关键调试方法论

1. **Host 内置截图是唯一可信的渲染验证手段**（captureScreenshot 从 swapchain image readback）
2. **ICD 文件日志**（icdLog via Win32 API）是唯一可信的 guest 侧调试手段
3. **分层验证**：先用固定色 shader 验证管线本身，再加 cbuffer 验证数据传输
4. **对比多帧截图**：frame 5 vs frame 150 的像素值对比是验证动画的正确方式
5. **从 host decoder 日志追踪完整数据链**：WriteMemory offset → BindBufferMemory offset → UpdateDescriptorSets buffer → BindDescriptorSets → Draw
