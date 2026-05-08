# FPS 长期运行退化排查全记录

**日期**: 2026-05-02 ~ 2026-05-07
**结果**: 根因是 `icd_generic_stub` 中 `fprintf(stderr, ...)` 的热路径锁争用（~30% CPU）
**最终修复**: 2 行改动——删除 `icd_generic_stub` 和 `lookupFunc` 中的 fprintf

---

## 问题描述

UltraKill 在相同场景下持续运行后 FPS 从 ~71 下降到 ~27（2.6x）。重新打开 Guest（新 host session）FPS 恢复到 71。关卡 restart（同 session）不恢复，返回菜单（DestroyDevice）恢复。

## 排查时间线

### 阶段一：怀疑 Host 侧资源泄漏 (Day 1-2)

**假设**: Host 侧 Vulkan 对象追踪 map（descriptorSets_, imageLayouts_ 等）随 destroy 不清理而积累。

**行动**:
- 添加 host 侧 DIAG dump：每 300 batch 打印全部 24 个 map 的 size
- 检查 `handleDestroyImage`、`handleDestroyBuffer`、`handleDestroyDescriptorPool`

**发现**:
- `imageLayouts_`/`imageFormats_`/`imageSizes_` DestroyImage 时从不 erase ✓ 已修
- `bufferUsageFlags_`/`bufferBindings_` DestroyBuffer 时从不 erase ✓ 已修
- `descriptorSets_` DestroyDescriptorPool 时从不 clear ✓ 已修
- `cbLastFence_` 只增不删 ✓ 已修
- `descSets` 从 22 增长到 4500，但 plateau 后不再涨
- 修复后 DIAG 显示 imgFmts==images-3（恒定差值=swapchain），确认泄漏消除

**结论**: 泄漏真实存在并已修复，但不是 FPS 退化的主因。descSets 在 batch=5400 后稳定在 4500，但 decode 耗时继续恶化。

### 阶段二：怀疑 ICD 侧 Guest 状态积累 (Day 2-3)

**假设**: Guest 侧 flushMappedMemory 或 mappedRegions/memoryShadows 随运行积累。

**行动**:
- 添加 ICD-DIAG：每 300 frame dump `mappedRegions.size()`, `memoryShadows.size()`, `bdaRecorded_` 等
- 测量 flushMappedMemory、encode、TCP send 耗时

**发现**:
- `memoryShadows` 稳定在 8 个（612MB），`mappedRegions` 稳定在 1 个
- flushMappedMemory 恒常 ~137μs，encode 恒常 ~8μs
- ICD 侧无任何积累效应

**结论**: ICD 侧稳定，不退化。

### 阶段三：对比录制 + 帧间隔分析 (Day 3-4)

**假设**: DXVK 随 session 老化产生更多命令，导致帧间隔变大。

**行动**:
- 给 host 加 GUI 按钮实现 toggle recording
- 在 FPS 好的时候（早期）和差的时候（晚期）分别录制命令流
- 写 Python 脚本解析 Venus 协议头，统计命令分布

**发现**:
- N1（早期 71 FPS）和 N2（晚期 27 FPS）的**命令量和分布完全相同**
  - N1: 279 batches, ~780 cmd/batch, ~452KB/batch
  - N2: 132 batches, ~768 cmd/batch, ~403KB/batch
  - 命令类型分布差异 <2%
- **帧间隔（TimingSeq 时间戳）**: N1=14.0ms, N2=36.8ms
- Host T2 帧间隔与 Guest 完全一致（37ms）
- Host decode 恒常 3-4ms

**结论**: DXVK 输出相同的命令，瓶颈不在命令录制。帧间隔在 host 侧。

### 阶段四：Readback Age + 协议分析 (Day 4-5)

**假设**: Host GPU 侧 readback/present 延迟或 AcquireNextImage 协议阻塞。

**行动**:
- 提取 host_err.txt 中的 `readback ready (deferred), age=Xms`
- 分析 AcquireNextImage 协议（发现当前是阻塞模式，之前 async 被回退）
- 临时恢复 async AcquireNextImage 测试

**发现**:
- **readback age: 8.5ms → 23ms（2.7x）**, 与帧间隔恶化比例完全吻合
- async AcquireNextImage 对 FPS 无帮助（瓶颈在 host 侧非协议）
- 新 guest session（新 host session）立即恢复到 ~8.5ms readback age
- descSets 在被新 session 中从 0 开始，老 session 中 3500+ 且 90% 是 stale（300+ 帧未绑定）

**结论**: Readback age 是瓶颈。根源是 host session 内 descriptor set 从 0→4000 积累。

### 阶段五：CPU Profiling 终结 (Day 5-6)

**假设**: 30% CPU 莫名其妙花在 vbox_vulkan.dll 中。

**行动**:
- 用户采集 WPR ETL traces（guest_degraded.etl 系列）
- WPA GUI 分析 ULTRAKILL.exe 进程 CPU 采样
- 展开 vbox_vulkan.dll → `icd_generic_stub` → `fprintf` 占 ~30% CPU（Weight 0.55 / RootWeight 1.8）

**发现**:
```cpp
static VkResult VKAPI_CALL icd_generic_stub() {
    fprintf(stderr, "[ICD] !!! generic_stub called !!!\n");  // ← 30% CPU
    return VK_SUCCESS;
}
```

DXVK 每帧**数千次**调用某个未实现的 Vulkan 函数，每次都走 `icd_generic_stub` → `fprintf`。stderr 的锁在 16 核 CPU 上激烈争用。

**修复**: 删除 `icd_generic_stub` 中的 `fprintf`。`lookupFunc` 中的 `fprintf(stderr, "[ICD] Stubbed: %s\n", pName)` 改为 `icdDbg`（仅写文件，无锁争用）。

## 修复汇总

| # | 文件 | 修复 | 影响 |
|---|------|------|------|
| 1 | `icd_dispatch.cpp:3860` | `icd_generic_stub` 删除 fprintf | **主要修复** — 30% CPU |
| 2 | `icd_dispatch.cpp:3908` | `lookupFunc` fprintf 改 icdDbg | 辅助 — 避免冷路径锁 |
| 3 | `vn_decoder.cpp` | DestroyImage/Buffer/DescPool 清理 map | 内存泄漏修复 |
| 4 | `vn_decoder.cpp` | BeginCB erase cbLastFence_ | 防止 map 膨胀 |
| 5 | `icd_dispatch.cpp` | vkResetDescriptorPool/FreeDescriptorSets 转发 | descSet 生命周期 |
| 6 | `vn_decoder.h/.cpp` | DIAG dump 基建 | 持续诊断 |

## 关键经验

1. **fprintf 在热路径是锁争用杀手**。DLL 中的 stderr 锁在高核心数 CPU 上可轻易吃掉 30% CPU。任何 stub/fallback 函数必须静默。

2. **CPU Profiling (WPA) 是终极工具**。当所有的 log-based 分析都找不到根因时，直接采样 CPU 看时间花在哪。5 分钟 WPR 录制 + WPA 展开调用栈搞定。

3. **对比录制（dump diff）是排除假设的利器**。N1 vs N2 命令完全相同 → 排除 DXVK 命令录制变化 → 转向 host 侧。

4. **逐层排除法**：Host 侧 → ICD 侧 → 录制对比 → 协议分析 → CPU profiling。每一层用数据说话。

5. **不要过度设计修复**。在确定根因前做了 descriptor set 清理、vector 优化、CB double-buffering、async AcquireNextImage——都是正确但非根因的优化。

6. **stub 函数是隐形雷**。未实现的 Vulkan 函数如果被频繁调用，静默返回 VK_SUCCESS 比打日志安全得多。
