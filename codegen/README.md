# Bridge Codegen

从 vk.xml 自动生成 Venus 兼容的 encoder/decoder 代码，替代手写序列化。

## 运行

```bash
python3 codegen/bridge_codegen.py
```

输出到 `common/venus/vn_gen_*.h`。生成文件提交到 git，不需要构建时依赖 Python。

## 当前状态

**已生成（Batch 1 — 纯标量+handle 命令，11 个）：**
vkCmdDraw, vkCmdDrawIndexed, vkCmdBindPipeline, vkCmdEndRenderPass,
vkCmdEndRendering, vkCmdSetCullMode, vkCmdSetFrontFace, vkEndCommandBuffer,
vkBindBufferMemory, vkBindImageMemory, vkCmdBindIndexBuffer

**待生成（需扩展 codegen）：** 51 个带指针/结构体参数的命令

## 后续扩展步骤

### Step 1: 支持可忽略指针参数
`vkAllocationCallbacks*` 在所有 vkCreate/vkDestroy 中出现但我们不传输（总是 NULL）。
codegen 应识别并跳过此参数。这解锁所有 vkDestroyXxx 和部分 vkCreateXxx。

### Step 2: 支持简单数组参数
如 `vkResetFences(device, fenceCount, const VkFence* pFences)` —
指针参数带 `len="fenceCount"` 属性，按 `[count] [elem0] [elem1] ...` 序列化。
解锁：vkResetFences, vkWaitForFences, vkCmdSetViewport, vkCmdSetScissor,
vkCmdBindVertexBuffers, vkCmdBindDescriptorSets, vkCmdPushConstants 等。

### Step 3: 支持简单结构体序列化
如 VkFenceCreateInfo { sType, pNext, flags } — 按成员顺序写入。
pNext 初期写 NULL 标记（u32 0）。
解锁：vkCreateFence, vkCreateSemaphore, vkCreateCommandPool,
vkCreateBuffer, vkCreateImage, vkCreateImageView, vkCreateSampler 等。

### Step 4: 支持嵌套结构体和复杂数组
如 VkWriteDescriptorSet（含 union VkDescriptorImageInfo/BufferInfo），
VkGraphicsPipelineCreateInfo（15+ 嵌套 sub-structs）。
这些是最复杂的，可能需要保持部分手写。

### Step 5: 集成到 encoder/decoder
每个 batch 完成后：
1. 在 `vn_encoder.h` 的 cmdXxx() 中调用 `vn_encode_vkXxx()`
2. 在 `vn_decoder.cpp` 的 handleXxx() 中用 `vn_decode_vkXxx()` 替代手写 read
3. 更新 `vn_command.h` 使用 Venus 标准命令 ID
4. 编译 + 运行 dx11_triangle 验证

### Step 6: CMake 集成
添加 custom_command 自动重新生成（可选，生成文件已入 git）。

## 设计原则

- **命令 ID**：使用 Venus VkCommandTypeEXT 标准值（从 VK_EXT_command_serialization.xml）
- **参数顺序**：严格按 vk.xml 声明顺序，与 Venus wire format 兼容
- **Handle 编码**：统一 uint64_t
- **帧格式**：保留 [cmd_type:u32][cmd_size:u32][payload]（比 Venus 多一个 cmd_size 用于 TCP 错误恢复）
- **Bridge 命令**：0x10000+ 范围的自定义命令（swapchain/memory/stream）保持手写
