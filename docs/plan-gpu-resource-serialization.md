# GPU 资源序列化计划 — 让 DXVK 原始 blit shader 正常工作

## 背景

DXVK 不直接渲染到 swapchain。它的渲染流程是：

```
DX11 Draw → DXVK 渲染到内部 VkImage → blit shader 采样该 image → 输出到 swapchain
```

blit shader (shader_45) 的简化路径（无 HUD/光标）：
```glsl
coord = int2(gl_FragCoord.xy) + push.src_offset - push.dst_offset;
color = imageLoad(s_image, coord);  // Set 1, Binding 0
// 可选色彩空间转换
output = color;
```

当前问题：ICD 对资源创建 API（vkCreateImage、vkAllocateMemory 等）全部是空 stub。
DXVK 的内部 image 从未在 Host GPU 上创建 → blit shader 采样不到数据 → 黑屏。

## 需要实现的 API（按执行顺序）

### 第一层：Image + Memory（让 DXVK 的内部 rendertarget 在 Host 上真实存在）

| API | 当前状态 | 需要做的 |
|-----|---------|---------|
| `vkCreateImage` | 空 stub（分配 handle） | 序列化 VkImageCreateInfo → Host 创建真实 VkImage |
| `vkAllocateMemory` | 空 stub | 序列化 allocationSize + memoryTypeIndex → Host 分配真实内存 |
| `vkBindImageMemory` | 空 stub | 序列化 image + memory + offset → Host 绑定 |
| `vkCreateImageView` | 空 stub | 序列化 VkImageViewCreateInfo → Host 创建真实 VkImageView |

编码格式设计：

```
vkCreateImage (cmd=50):
  u64 deviceId, u64 imageId
  u32 imageType, u32 format, u32 width, u32 height, u32 depth
  u32 mipLevels, u32 arrayLayers, u32 samples
  u32 tiling, u32 usage, u32 sharingMode

vkAllocateMemory (cmd=21):
  u64 deviceId, u64 memoryId
  u64 allocationSize, u32 memoryTypeIndex

vkBindImageMemory (cmd=48):
  u64 deviceId, u64 imageId, u64 memoryId, u64 memoryOffset

vkCreateImageView (cmd=52):
  u64 deviceId, u64 viewId, u64 imageId
  u32 viewType, u32 format
  u32 aspectMask, u32 baseMipLevel, u32 levelCount, u32 baseArrayLayer, u32 layerCount
```

### 第二层：Descriptor Set（让 blit shader 能绑定纹理）

| API | 当前状态 | 需要做的 |
|-----|---------|---------|
| `vkCreateDescriptorPool` | 空 stub | 序列化 pool sizes → Host 创建 |
| `vkAllocateDescriptorSets` | 空 stub（分配 handle） | 序列化 pool + layouts → Host 分配 |
| `vkUpdateDescriptorSets` | 空 stub | 序列化 write descriptor sets → Host 更新 |
| `vkCmdBindDescriptorSets` | 空 stub | 序列化 bind 参数 → Host 录制 |

编码格式设计：

```
vkCreateDescriptorPool (新 cmd):
  u64 deviceId, u64 poolId
  u32 maxSets, u32 poolSizeCount
  [u32 type, u32 count] * poolSizeCount

vkAllocateDescriptorSets (cmd=已有但需要增强):
  u64 deviceId, u64 poolId
  u32 setCount
  [u64 layoutId, u64 setId] * setCount

vkUpdateDescriptorSets (关键且复杂):
  u64 deviceId
  u32 writeCount
  for each write:
    u64 dstSetId, u32 dstBinding, u32 dstArrayElement
    u32 descriptorCount, u32 descriptorType
    // 根据 type 决定数据：
    if IMAGE_SAMPLER/SAMPLED_IMAGE/STORAGE_IMAGE:
      [u64 samplerId, u64 imageViewId, u32 imageLayout] * descriptorCount
    if UNIFORM_BUFFER/STORAGE_BUFFER:
      [u64 bufferId, u64 offset, u64 range] * descriptorCount

vkCmdBindDescriptorSets (cmd 需要新增):
  u64 cbId, u32 bindPoint, u64 layoutId
  u32 firstSet, u32 setCount
  [u64 setId] * setCount
  u32 dynamicOffsetCount
  [u32 offset] * dynamicOffsetCount
```

### 第三层：Buffer（顶点数据、常量缓冲区 — 后续更复杂场景需要）

这一层对简单三角形测试不是必需的（顶点用 SV_VertexID 硬编码），
但对带 vertex buffer 的渲染必需。暂时跳过。

| API | 说明 |
|-----|------|
| `vkCreateBuffer` | Buffer 创建 |
| `vkBindBufferMemory` | Buffer 内存绑定 |
| `vkMapMemory` + 数据传输 | 将 CPU 数据传到 Host GPU |
| `vkCmdBindVertexBuffers` | 绑定顶点缓冲区 |
| `vkCmdCopyBufferToImage` | Buffer → Image 数据传输 |

## 实现顺序

### Phase 1：Image + Memory + ImageView
修改文件：vn_command.h, vn_encoder.h, icd_dispatch.cpp, vn_decoder.cpp/h

目标：DXVK 的内部 rendertarget image 在 Host GPU 上真实存在。

注意事项：
- Host GPU 的 memoryTypeIndex 和 Guest ICD 报告的不同。
  ICD 报告了 2 种 memory type（device local + host visible）。
  Host 需要映射到 Host GPU 实际支持的 memory type。
  简单做法：根据 ICD 的 type 选择 Host 对应的 type（device local → Host 的 device local）。
- Image format：ICD 报告支持所有格式，但 Host GPU 可能不支持某些。
  先直接传 format，如果创建失败再处理。

### Phase 2：DescriptorPool + AllocateDescriptorSets + UpdateDescriptorSets
目标：blit shader 的 descriptor set 正确指向内部 image 的 view。

注意事项：
- vkUpdateDescriptorSets 的序列化最复杂（多种 descriptor type 有不同数据）。
  对 blit shader 只需要支持 SAMPLED_IMAGE 和 COMBINED_IMAGE_SAMPLER。
- Sampler 创建（vkCreateSampler）也需要转发——blit shader 用 sampler 数组。

### Phase 3：CmdBindDescriptorSets
目标：在 command buffer 中绑定 descriptor set。

### Phase 4：Sampler 创建
目标：DXVK 的采样器在 Host GPU 上真实存在。

| API | 说明 |
|-----|------|
| `vkCreateSampler` | 转发创建参数（filter、addressMode 等） |

## 验证方式

每个 phase 完成后：
1. 编译通过
2. 运行 dx11_triangle 测试
3. 用 disasm_cmdstream.py 检查新命令是否正确编码
4. 用 WGC 截图检查窗口内容
5. 最终目标：WGC 显示橙色三角形（来自 DXVK 原始渲染，不是 builtin shader）

## 复杂度估算

| Phase | 新增/修改文件 | 新增 encoder 方法 | 新增 decoder handler | 估算行数 |
|-------|-------------|-----------------|-------------------|---------|
| Phase 1 | 5 | 4 | 4 | ~300 |
| Phase 2 | 5 | 3 | 3 | ~400 |
| Phase 3 | 5 | 1 | 1 | ~100 |
| Phase 4 | 5 | 1 | 1 | ~100 |
| **合计** | | **9** | **9** | **~900** |

## DXVK blit shader 最小依赖

对于简单三角形测试（无 HUD/光标），blit shader 只需要：
- **Set 1, Binding 0**: s_image (StorageImage 2D) — DXVK 内部 rendertarget
- **Push Constants**: src_offset, src_extent, dst_offset（已实现转发）

Set 0 的 sampler 数组和 Set 1 的其他 binding（gamma、hud、cursor）在简单测试中不被采样（spec constant 控制分支跳过）。但 descriptor set 仍需正确分配和绑定，否则 validation layer 报错。
