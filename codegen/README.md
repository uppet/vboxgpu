# Bridge Codegen

从 vk.xml 自动生成 Venus 兼容的 encoder/decoder 代码，替代手写序列化。

## 运行

```bash
python3 codegen/bridge_codegen.py
```

输出到 `common/venus/vn_gen_*.h`。生成文件提交到 git，不需要构建时依赖 Python。

## 当前状态

**已生成（32/62 个 API）：**

Batch 1 — 纯标量+handle 命令（11 个）：
vkCmdDraw, vkCmdDrawIndexed, vkCmdBindPipeline, vkCmdEndRenderPass,
vkCmdEndRendering, vkCmdSetCullMode, vkCmdSetFrontFace, vkEndCommandBuffer,
vkBindBufferMemory, vkBindImageMemory, vkCmdBindIndexBuffer

Batch 2 — 可忽略指针 (VkAllocationCallbacks*) 解锁的命令（15 个）：
vkDestroyBuffer, vkDestroyCommandPool, vkDestroyDescriptorPool,
vkDestroyDescriptorSetLayout, vkDestroyFence, vkDestroyFramebuffer,
vkDestroyImage, vkDestroyImageView, vkDestroyPipeline,
vkDestroyPipelineLayout, vkDestroyRenderPass, vkDestroySampler,
vkDestroySemaphore, vkDestroyShaderModule, vkFreeMemory

Batch 3 — 简单数组参数解锁的命令（6 个）：
vkResetFences, vkWaitForFences, vkCmdBindDescriptorSets,
vkCmdBindVertexBuffers, vkCmdPushConstants, vkCmdUpdateBuffer

**待生成（需扩展 codegen）：** 30 个带结构体/输出指针/可选数组的命令

## codegen 支持的参数类型

| 类型 | 说明 | 示例 |
|------|------|------|
| handle | Vulkan handle → uint64_t | VkDevice, VkBuffer |
| scalar | 标量 → uint32_t/uint64_t/float | uint32_t, VkDeviceSize, VkBool32 |
| ignorable | 指针参数，跳过不传输 | VkAllocationCallbacks* |
| handle_array | handle 数组，count+元素 | const VkFence* pFences |
| scalar_array | 标量数组，count+元素 | const uint32_t* pDynamicOffsets |
| byte_data | 字节数据，size+writeBytes | const void* pValues |

## 后续扩展步骤

### Step 3: 支持简单结构体序列化
如 VkFenceCreateInfo { sType, pNext, flags } — 按成员顺序写入。
pNext 初期写 NULL 标记（u32 0）。
解锁：vkCreateFence, vkCreateSemaphore, vkCreateCommandPool,
vkCreateBuffer, vkCreateImage, vkCreateImageView, vkCreateSampler 等。
还需处理输出指针参数（vkCreate* 的最后一个 VkXxx* 参数）。

### Step 4: 支持嵌套结构体和复杂数组
如 VkWriteDescriptorSet（含 union VkDescriptorImageInfo/BufferInfo），
VkGraphicsPipelineCreateInfo（15+ 嵌套 sub-structs）。
以及可选数组参数（vkCmdBindVertexBuffers2 的 pSizes/pStrides）。
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
- **可忽略参数**：VkAllocationCallbacks* 等在我们的 bridge 中总为 NULL 的参数直接跳过
- **数组编码**：count 作为普通标量写入，紧跟数组元素；decoder 用 std::vector 存储
