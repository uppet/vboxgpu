# 从动画三角形到纹理三角形

## 目标
在 vertex buffer 三角形上叠加棋盘格纹理采样，验证完整的 DX11 资源管线：
vertex buffer + texture + sampler + constant buffer 通过 ICD → TCP → Host Vulkan 渲染。

## 已完成的基础设施

### 编解码全链路（6 个新命令）
| 命令 | ID | 状态 |
|------|----|------|
| vkCmdBindVertexBuffers / BindVertexBuffers2 | 0x100D | ✅ 含 VK_WHOLE_SIZE 修复 |
| vkCmdBindIndexBuffer | 0x100E | ✅ |
| vkCmdDrawIndexed | 0x100F | ✅ 替换旧 cmdDraw hack |
| vkCmdCopyBuffer / CopyBuffer2KHR | 0x1010 | ✅ 含 staging flush |
| vkCmdCopyBufferToImage / 2KHR | 0x1011 | ✅ 含 staging flush（bufferOffset 截断为 u32 待修） |
| vkCmdUpdateBuffer | 0x1012 | ✅ |

### Pipeline vertex input state 序列化
- Encoder 追加 bindingCount + attributeCount 在 colorFmt 之后
- Decoder 用 `r.remaining() >= 4` 向后兼容旧命令流
- Legacy overload 保持 guest_sim / host_cmd 兼容
- 验证：host 日志确认 1 binding + 2 attrs (R32G32B32_SFLOAT + R32G32_SFLOAT)

### Vulkan 1.3 API 变体注册
- `vkCmdCopyBuffer2` / `vkCmdCopyBuffer2KHR`（DXVK 实际使用的路径）
- `vkCmdCopyBufferToImage2` / `vkCmdCopyBufferToImage2KHR`

### 动态状态
- Pipeline 动态状态含 `VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE`
- DXVK 在 pipeline 中 stride=0，通过 BindVertexBuffers2 动态传递

## 已解决的关键问题

### 1. Staging buffer 数据丢失（vertex buffer 全黑）
**根因**：CopyBuffer/CopyBufferToImage 前没有 flush staging buffer 的 shadow memory。
DXVK 用 persistent mapping 写 staging buffer，永不 unmap。flushMappedMemory 跳过 >64KB 区域。

**修复**：在 CopyBuffer/CopyBuffer2/CopyBufferToImage/2 编码前调用 `flushBufferRange` 将 source buffer 的 shadow memory 上传到 host。

### 2. BindVertexBuffers2 size=0（顶点数据不可读）
**根因**：ICD 的 `icd_vkCmdBindVertexBuffers2` 对 `pSizes=NULL` 发送 0 而非 `VK_WHOLE_SIZE`。
Host 用 size=0 调 `vkCmdBindVertexBuffers2` → GPU 绑定 0 字节 vertex buffer。

**修复**：`pSizes=NULL` 时发送 `VK_WHOLE_SIZE` (~0ULL)。

### 3. VkFormatProperties3 未填充（SRV 创建失败 E_INVALIDARG）
**根因**：DXVK 通过 `vkGetPhysicalDeviceFormatProperties2` 的 pNext 查询 `VkFormatProperties3`（Vulkan 1.3，sType=1000360000）。
我们没填写这个结构体 → DXVK 读到全零 → 认为所有 format 不支持 SRV → `CreateShaderResourceView` 返回 E_INVALIDARG。

**修复**：在 FormatProperties2 handler 中遍历 pNext，找到 sType=VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3 时复制 32-bit flags 到 64-bit。

### 4. 深度格式 feature bits 错误（DXVK 初始化崩溃）
**根因**：所有 format 统一返回 `COLOR_ATTACHMENT_BIT`，但深度格式（D16_UNORM..D32_SFLOAT_S8_UINT）不应有此标志。
DXVK 查询第一个深度格式时检测到不合法的 feature 组合，abort 初始化。

**修复**：根据 format 范围区分深度/颜色，深度格式返回 `DEPTH_STENCIL_ATTACHMENT_BIT` 而非 `COLOR_ATTACHMENT_BIT`。

### 5. VK_NVX_image_view_handle 必需（设备创建失败）
**根因**：DXVK 依赖 `VK_NVX_image_view_handle` 扩展。移除后设备创建返回 E_FAIL。
DXVK 通过 `vkGetImageViewHandleNVX` 获取 ImageView handle 用于 descriptor 绑定，绕过标准 `vkCreateImageView`。

**修复**：实现 `vkGetImageViewHandleNVX` 返回 ICD 内部 handle ID，`vkGetImageViewAddressNVX` 返回 handle 作为 device address。

### 6. BeginRendering swapchain 重定向逻辑（全黑根因）
**根因**：DXVK 每帧有两个 pass：
1. **Draw pass**（imageViewId≠0）→ 渲染到 internal render target (image 12, fmt=R8G8B8A8_UNORM)
2. **Blit pass**（imageViewId=0）→ fullscreen blit，从 image 12 采样复制到 swapchain

早期所有 pass 无条件重定向到 swapchain → draw pass 绕过 internal target → blit 读到空数据 → 全黑。

**修复**：只在 `imageViewId==0`（blit/present pass）或 view lookup 失败时重定向到 swapchain。`imageViewId!=0` 且 view 存在时渲染到原始目标。

关联修复：pipeline colorFormat 只对 blit pipeline（0 vertex bindings）覆盖为 swapchain 格式，draw pipeline 保持原始格式匹配 internal render target。

### 7. ICD dispatchable handle loader magic
**根因**：Vulkan loader Debug 构建要求 dispatchable handle 首 4 字节为 `0x01CDC0DE`，否则 assert 失败。

**修复**：`makeDispatchable()` 写入 `ICD_LOADER_MAGIC` 替代 `nullptr`。

## 验证结果
- Host captureScreenshot (dbg_frame5.bmp) 显示倒三角形（Y-flip viewport）
- 像素值在亮 (R≈255) 和暗 (R≈150) 间交替 — 棋盘格纹理采样正确
- HSV 变色 tint 正常（frame 5 = 橙红色）

## 已知编解码问题（待修）
- `CopyBufferToImage` 的 `bufferOffset` 被截断为 `uint32_t`（应为 `uint64_t`）。对 >4GB 偏移的大纹理会出错，当前 64x64 纹理不受影响。

## 测试程序 (dx11_triangle)
- Vertex buffer: 3 vertices (pos.xyz + uv.xy = 20 bytes/vertex)
- Texture: 64x64 R8G8B8A8_UNORM 棋盘格
- PS: `tex.Sample(smp, uv) * tint + uvColor * 0.3 * tint`（纹理+UV fallback+HSV 变色）
- SRV 创建需要显式 desc（非 nullptr）
- `if (srv) PSSetShaderResources` — SRV 失败时不绑定
