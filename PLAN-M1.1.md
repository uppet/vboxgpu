# 阶段一 MVP：Host Vulkan 三角形渲染器

## Context

项目处于零代码状态。需要先建立可编译、可运行的最小 Host 端程序，验证：
1. CMake + MSVC 构建链路通畅（通过 delegate-runner 触发 Windows 编译）
2. Vulkan SDK 链接正常
3. 能在 Host 窗口中渲染出一个三角形

这是 M1.1 的核心目标，也是后续所有工作的基础。暂不涉及 Venus 协议——先确保 Vulkan 渲染管线本身能跑通。

## 环境需求清单

请手动确认/准备以下项目：

- [ ] Visual Studio 2022 + MSVC C++ 桌面开发工具
- [ ] Vulkan SDK（已有 1.3.216.0），确认环境变量 `VULKAN_SDK` 已设置
- [ ] CMake 在 Windows PATH 中可用（`cmake --version` 能跑通）
- [ ] glslc（Vulkan SDK 自带，在 `%VULKAN_SDK%\Bin\glslc.exe`）
- [ ] delegate-runner MCP 可用

## 交付物

```
vboxgpu/
├── CMakeLists.txt                  # 顶层 CMake
├── host/
│   ├── CMakeLists.txt              # Host 子项目
│   └── src/
│       ├── main.cpp                # 入口：创建窗口 + Vulkan 初始化 + 主循环
│       ├── vk_bootstrap.h/cpp      # Vulkan 实例/设备/交换链创建
│       ├── vk_pipeline.h/cpp       # 渲染管线 + 内嵌 SPIR-V 三角形 Shader
│       └── vk_renderer.h/cpp       # 命令录制 + 帧提交 + 呈现
└── shaders/
    ├── triangle.vert               # 顶点着色器 (GLSL)
    └── triangle.frag               # 片段着色器 (GLSL)
```

## 技术决策

| 决策 | 选择 | 理由 |
|------|------|------|
| 窗口库 | Win32 API 原生 | 零额外依赖，Host 目标就是 Windows |
| Shader 编译 | 构建时 glslc → SPIR-V，嵌入 C++ 数组 | 运行时无需 shader 文件 |
| Vulkan 版本 | 1.2 | 与 SDK 1.3.216.0 兼容，不用最新特性 |
| 验证层 | Debug 构建自动开启 | 开发期自动捕获错误 |

## 实施步骤

### Step 1: 顶层 CMake + 项目骨架
- 创建 `CMakeLists.txt`，find_package(Vulkan)，添加 host 子目录
- 创建 `host/CMakeLists.txt`，编译为 Win32 可执行文件

### Step 2: Win32 窗口 + Vulkan 初始化 (`main.cpp` + `vk_bootstrap.cpp`)
- Win32 窗口创建（固定 800x600）
- VkInstance（启用验证层）
- VkSurfaceKHR（Win32 surface）
- 选择物理设备 + 创建逻辑设备
- 创建交换链

### Step 3: 渲染管线 (`vk_pipeline.cpp`)
- 内嵌预编译的三角形 SPIR-V shader
- 创建 RenderPass、PipelineLayout、GraphicsPipeline
- 创建 Framebuffer

### Step 4: 渲染循环 (`vk_renderer.cpp`)
- 命令池 + 命令缓冲区
- 每帧：acquire image → record commands → submit → present
- Fence/Semaphore 同步

### Step 5: Shader 源码 + 编译集成
- 写 GLSL 顶点/片段着色器
- CMake 中用 glslc 编译为 .spv，生成 C++ 头文件嵌入

### Step 6: 构建验证
- 通过 delegate-runner 执行 `cmake -B build -G "Visual Studio 17 2022"`
- 通过 delegate-runner 执行 `cmake --build build --config Debug`
- 确认编译通过，生成 exe

### Step 7: 运行验证
- 通过 delegate-runner 运行生成的 exe
- 确认窗口弹出并显示彩色三角形

## 验证方式

1. **编译通过** — delegate-runner 执行 cmake build 无报错
2. **运行通过** — exe 启动后弹出窗口，显示三角形（可通过截图确认）
3. **验证层无报错** — Debug 输出中无 Vulkan validation error
