# 花屏（Ghost Frame）修复事后总结

## 问题描述

SortTheCourt 真实游戏运行时，每隔约 25 帧出现一帧异常画面（花屏）。花屏的视觉特征：

- **纹理内容完全正确**，贴图、颜色均无异常
- **位置、变换、旋转错位**，像是用了错误的 MVP 矩阵渲染了正确的对象
- 花屏仅持续单帧，前后帧立即恢复正常

576 帧录制中检测出 23 帧花屏（约 4%）。

## 根因分析

### 时序竞争（CPU 写 vs GPU 读）

Host 侧 decoder 按批次（batch）顺序处理命令流。每个 batch 内典型顺序为：

```
WriteMemory(mem, offset, transform_matrix_data)
BeginCommandBuffer
CmdBindDescriptorSets  ← 引用 mem+offset 处的 uniform buffer
CmdDraw
EndCommandBuffer
vkQueueSubmit(fence)   ← GPU 异步开始执行，CPU 立即返回
```

**问题出在相邻 batch 之间：**

```
Batch N：
  WriteMemory(mem42, 0, transform_A)    ← 写入 HOST_COHERENT GPU 内存
  vkQueueSubmit(fence_N)                ← GPU 开始异步读 transform_A
  [CPU 立即返回，接收下一个 batch]

Batch N+1：
  WriteMemory(mem42, 0, transform_B)    ← GPU 可能仍在读 transform_A！覆写！
  BeginCommandBuffer ...
  vkQueueSubmit(fence_N+1)             ← GPU 看到的可能是 transform_B 渲染帧 N
```

由于内存是 `VK_MEMORY_PROPERTY_HOST_COHERENT_BIT`，CPU 写入对 GPU **立即可见**，没有任何缓冲。

### 为什么 BeginCommandBuffer 里的 fence 等待无效？

`handleBeginCommandBuffer` 中有对 `cbLastFence_` 的等待（确保 CB 可以被 reset），但这个等待发生在 **WriteMemory 之后**。命令流顺序决定了：WriteMemory 总是先执行，fence 等待永远保护不到写入操作。

### 为什么花屏率只有 4%？

TCP 往返时延（Host 发送 response → Guest 处理下一帧 → Host 收到 batch N+1）通常给 GPU 留出足够时间完成渲染。只有 GPU 渲染比 TCP 往返更慢时（复杂场景）才会触发竞争，因此概率较低但真实存在。

## 修复方案

### P0：QueueSubmit 全程 encoder lock（Guest 侧）

防止多线程下 `flushMappedMemory`（发送多条 WriteMemory）与 `CmdCopyBufferToImage` 的命令交错。

```cpp
// icd_dispatch.cpp
// 将 mutex 升级为 recursive_mutex（支持 flushMappedMemory 内部再次加锁）
#define ENC_LOCK std::lock_guard<std::recursive_mutex> _enc_lock(g_icd.encoder.mutex_)

static VkResult VKAPI_CALL icd_vkQueueSubmit(...) {
    ENC_LOCK;  // flush + submit 作为原子单元
    g_icd.flushMappedMemory();
    // ... encode submit commands
}
```

### P1b：跨 batch fence 等待（Host 侧，根因修复）

在每个 batch 的第一次 WriteMemory 前，等待上一个 batch 的最后一次 QueueSubmit 完成。

```cpp
// vn_decoder.h
VkFence lastBatchFence_ = VK_NULL_HANDLE;
bool    lastBatchWaitPending_ = false;

// vn_decoder.cpp — handleQueueSubmit
if (submitFence != VK_NULL_HANDLE) {
    lastBatchFence_ = submitFence;
    lastBatchWaitPending_ = true;  // 下一个 batch 的第一个 WriteMemory 需要等待
}

// vn_decoder.cpp — handleWriteMemory
if (lastBatchWaitPending_ && lastBatchFence_ != VK_NULL_HANDLE) {
    vkWaitForFences(device_, 1, &lastBatchFence_, VK_TRUE, UINT64_MAX);
    lastBatchWaitPending_ = false;  // 本 batch 内后续 WriteMemory 不再等待
}
```

**设计要点：**
- 每个 batch 只等一次（第一个 WriteMemory 等，之后设 flag = false）
- 大多数情况下 fence 已经 signaled（GPU 在 TCP 往返期间就完成了），`vkWaitForFences` 立即返回，性能损失极小
- 只在 GPU 真正落后时才会阻塞，这是必须阻塞的时刻

## 验证结果

| 测试 | 花屏帧数 |
|------|---------|
| P0 fix only（sc_p0 replay） | **23 / 576**（4.0%） |
| P0 + P1b fix（sc_p1b replay） | **0 / 576**（0%） |

同一录制（sc_p0.bin）在新 Host 下 replay，花屏完全消失。

## 辅助工具

### `scripts/detect_ghostframe.py` — 自动花屏检测

**关键算法：双向 diff 突峰（bilateral spike）**

```
bi_diff[i] = min(diff(frame_{i-1}, frame_i), diff(frame_i, frame_{i+1}))
```

花屏帧特征：`bi_diff[i]` 远大于邻居中位数，且邻居 `bi_diff` 均低（不是连续场景转场）。

最初尝试"散乱像素（disorder）"指标完全无效——本项目的花屏是变换矩阵错误导致的大块整体位移，disorder=0，必须用双向突峰才能检出。

**用法：**
```bash
# 用 replay --save-frames 生成帧
vbox_host_server.exe --replay dump.bin --save-frames S:/bld/vboxgpu/frames/dir

# 运行检测
python3 scripts/detect_ghostframe.py --frames-dir /mnt/s/bld/vboxgpu/frames/dir --only-flagged-png
```

### `--save-frames DIR`（Host replay 模式）

`vbox_host_server.exe --replay dump.bin --save-frames DIR` 在每个 Present batch 后截图，输出 `frame_NNNN.bmp`。

**修复一个 bug：** Present 扫描代码用 `p += 8 + sz`，但 `sz` 已含 8 字节 header，应为 `p += sz`。这个 bug 导致 Present 始终扫不到，frames 目录为空。

## 经验教训

1. **HOST_COHERENT 内存不等于"安全"：** CPU 写入立即对 GPU 可见，没有任何自动同步。只要 GPU 还在读取某块内存，CPU 就不能写。

2. **花屏的视觉特征决定检测算法：** 像素散乱 → disorder 指标；变换矩阵错误 → bilateral spike 指标。先肉眼确认花屏类型再选算法。

3. **replay 是调试的关键：** 相同命令流在不同 Host 版本下 replay，可以精确对比修复效果，无需每次重新运行游戏。

4. **Opus 的架构洞察：** BeginCommandBuffer 里的 fence 等待在 WriteMemory 之后，无法保护写入。这是 decoder 命令处理顺序决定的，纯粹靠人工 review 很难发现。
