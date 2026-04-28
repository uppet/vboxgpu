# UI 自动化测试框架：基建进展与经验总结

## 概述

本文档记录 `tests/automation/` 目录下的 UI 自动化测试基建，以及首次用其驱动 SortTheCourt 完整游玩流程所积累的经验。

---

## 已建立的基建

### Python 端（`tests/automation/`）

| 文件 | 职责 |
|------|------|
| `capture.py` | WGC 窗口截图 + pHash 图像比较 + **host BMP 按需截图**（`capture_host_frame`）|
| `input.py` | 窗口输入：鼠标点击（SetCursorPos + SendInput + PostMessage 三合一）+ 键盘按键（keybd_event）|
| `process.py` | HostServer / GuestProcess 生命周期管理（context manager，自动启停、日志重定向）|
| `runner.py` | CLI 入口，`python runner.py sort_the_court --rounds N` |
| `task_yynyyy.py` | Vision-guided 游玩任务脚本（截图 + 分析 + 决策示范）|

### Host 端（`host/src/vn_decoder.cpp`）

新增**按需截图机制**（flag-file 信令）：

```
Python 创建 dbg_capture_request
  → Host 在下一帧 Present 时检测到 → 写 dbg_latest.bmp → 删除 flag
    → Python 读取 BMP → 转 PNG → 供模型分析
```

这使得自动化脚本可以在游戏运行中的任意时刻获取 Host 侧真实渲染帧，无需依赖 WGC（WGC 对我们的 Vulkan ICD 截不到画面）。

---

## 首次完整游玩测试（SortTheCourt Y,Y,N,Y,Y）

### 验证结论

- **Continue 按钮**：鼠标 SetCursorPos + SendInput 组合可靠触发 UnityEngine.UI 按钮 ✓
- **Y/N 游戏选择**：键盘 Y/N 键（keybd_event）被 Unity 正确接收，有明确视觉反馈 ✓
  - Sneaky Girl YES → +1 人口 ✓
  - Military General YES → -50 金 +3 幸福 ✓
  - 多个 NPC 和场景切换正常显示 ✓
- **Host BMP 按需截图**：全程正常工作（ESC 后 host 退出导致最后一帧失败，属预期）

### 观察到的时序挑战

游戏内有若干状态不适合固定等待：

| 状态 | 现象 | 影响 |
|------|------|------|
| 日结算屏（Day N complete）| 两个问题之间插入全屏结算 | 固定 sleep 可能把键按到结算屏而非问题 |
| 访客间隙 | 当前访客离开、下一个还未进场 | N 键可能打在空档 |
| 答题动画延迟 | thumbs up / score change 动画 >1.8s | after-snap 截到的可能是下一题开始而非上题结果 |

---

## 经验与设计原则

### 1. Unity 鼠标输入需要三合一

仅 `PostMessage(WM_LBUTTONDOWN)` 对 UnityEngine.UI 无效。必须：
1. `SetCursorPos` — 移动物理光标
2. `SendInput(MOUSEEVENTF_LEFTDOWN/UP)` — 触发 UI 系统
3. `PostMessage(WM_LBUTTONDOWN/UP)` — 触发 in-game Unity Input 系统

对于 Y/N 等有键盘提示（游戏 UI 右上角显示 Y/N）的操作，**优先用键盘**，比鼠标坐标更可靠。

### 2. 两阶段工作流

```
阶段一：Vision-guided（调试 / 校准）
  run → snap → 模型看图 → 分析坐标和时序 → 修改脚本 → 重跑

阶段二：Blind playback（稳定后 / CI）
  固定时序 + 固定动作，无截图，无模型介入
```

`task_yynyyy.py` 中把所有 `snap()` 调用删除，即变为 blind 脚本。

**两阶段分工**：

| | Vision 模式 | Blind 模式 |
|--|--|--|
| 用途 | 第一次跑、调参、排查问题 | 回归测试、兼容性 CI |
| 截图 | 每步截 + 模型分析 | 无 |
| Context 消耗 | 高（图片重） | 极低 |
| 可靠性 | 强（能感知异常状态） | 依赖时序稳定 |

### 3. Context 效率

这次对话 context 消耗较大，主要来源：

- **PNG 图片**：每张图片把视觉数据嵌入 context，8 张约占大量空间
- **状态轮询**：两次运行 ~28 次 `get_task_status` 调用
- **会话开头的长 summary**：上次 context compaction 产生

**优化方向**：把「运行游戏 + 截图 + 图片分析」打包给 sub-agent，主 agent 只收文字摘要，context 可降低 60–70%。

```
主 agent：决策 + 代码修改
   ↓ 委托
Sub-agent：run_command + 轮询 + 读图 + 返回文字摘要
```

### 4. 新游戏接入流程（推荐）

1. 用 vision 模式跑一次，确认截图和操作坐标
2. 用模型读 PNG 分析 UI 元素位置（Y/N 气泡、按钮等）
3. 校准时序（日结算、访客间隙、动画延迟）
4. 固化为 blind 脚本，删除 `snap()` 调用
5. 加入 CI / 回归测试集

---

## 待改进

- [ ] `process.py` HostServer：轮询 `auto_host_err.txt` 确认 "TCP server listening"，替代固定 `sleep(2)`
- [ ] Vision 模式：检测 Y/N 气泡出现再按键（pHash 或 mean 阈值），消除时序盲区
- [ ] `capture_host_frame`：ESC 后 host 退出导致最后截图失败，可在 GuestProcess 退出前截图
- [ ] Sub-agent 封装：把游玩循环封成 general-purpose agent 调用，保护主对话 context
