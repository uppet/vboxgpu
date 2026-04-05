// Host TCP server: receives Venus command streams from Guest, decodes and renders.
// Also supports --replay <dump.bin> to replay a recorded command stream with screenshot.

#include "vn_decoder.h"
#include "../../common/transport/tcp_transport.h"
#include <cstdio>
#include <vector>
#include <thread>
#include <atomic>
#include <cstring>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

static LONG WINAPI hostCrashHandler(EXCEPTION_POINTERS* ep) {
    CreateDirectoryA("S:\\bld\\vboxgpu\\dumps", NULL);
    HANDLE hFile = CreateFileA("S:\\bld\\vboxgpu\\dumps\\host_crash.dmp",
        GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
            MiniDumpNormal, &mei, NULL, NULL);
        CloseHandle(hFile);
        fprintf(stderr, "[Host] Crash dump: S:\\bld\\vboxgpu\\dumps\\host_crash.dmp\n");
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static constexpr uint32_t WINDOW_WIDTH = 800;
static constexpr uint32_t WINDOW_HEIGHT = 600;
static std::atomic<bool> g_running{true};

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CLOSE:
    case WM_DESTROY:
        g_running = false;
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) { g_running = false; PostQuitMessage(0); }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

// --- Replay mode ---
struct DumpBatch { std::vector<uint8_t> data; };

static std::vector<DumpBatch> loadDumpFile(const char* path) {
    std::vector<DumpBatch> batches;
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[Replay] Cannot open: %s\n", path); return batches; }
    while (true) {
        uint32_t sz;
        if (fread(&sz, sizeof(sz), 1, f) != 1) break;
        DumpBatch batch;
        batch.data.resize(sz);
        if (fread(batch.data.data(), 1, sz, f) != sz) break;
        batches.push_back(std::move(batch));
    }
    fclose(f);
    fprintf(stderr, "[Replay] Loaded %zu batches from %s\n", batches.size(), path);
    return batches;
}

static int replayMode(const char* dumpPath) {
    auto batches = loadDumpFile(dumpPath);
    if (batches.empty()) { fprintf(stderr, "[Replay] No batches.\n"); return 1; }

    HINSTANCE hInstance = GetModuleHandle(nullptr);
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "VBoxGPUReplay";
    RegisterClassExA(&wc);

    RECT rect = { 0, 0, (LONG)WINDOW_WIDTH, (LONG)WINDOW_HEIGHT };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName,
                                "VBox GPU Bridge - Replay",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                rect.right - rect.left, rect.bottom - rect.top,
                                nullptr, nullptr, hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);

    VulkanContext vk{};
    try {
        createInstance(vk);
        createSurface(vk, hwnd, hInstance);
        pickPhysicalDevice(vk);
        createLogicalDevice(vk);
    } catch (const std::exception& e) {
        fprintf(stderr, "Vulkan Init Error: %s\n", e.what());
        return 1;
    }

    VnDecoder decoder;
    decoder.init(vk.physicalDevice, vk.device, vk.graphicsQueue, vk.graphicsFamily, vk.surface);

    // Execute all batches once
    for (size_t i = 0; i < batches.size() && g_running; i++) {
        MSG msg{};
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!g_running) break;
        if (!decoder.execute(batches[i].data.data(), batches[i].data.size())) {
            fprintf(stderr, "[Replay] Batch %zu failed\n", i);
            break;
        }
    }

    // Screenshot
    std::string ssPath = dumpPath;
    auto dotPos = ssPath.rfind('.');
    if (dotPos != std::string::npos) ssPath = ssPath.substr(0, dotPos);
    ssPath += "_screenshot.bmp";
    fprintf(stderr, "[Replay] All batches done. Capturing screenshot.\n");
    decoder.captureScreenshot(ssPath.c_str());

    fprintf(stderr, "[Replay] Keeping window open (ESC to exit).\n");
    while (g_running) {
        MSG msg{};
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        Sleep(50);
    }

    decoder.cleanup();
    cleanupVulkan(vk);
    DestroyWindow(hwnd);
    UnregisterClassA(wc.lpszClassName, hInstance);
    return 0;
}

int main(int argc, char* argv[]) {
    SetUnhandledExceptionFilter(hostCrashHandler);

    // Check --replay mode first
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--replay") == 0 && i + 1 < argc) {
            return replayMode(argv[++i]);
        }
    }

    uint16_t port = DEFAULT_PORT;
    const char* dumpPath = nullptr;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
            dumpPath = argv[++i];
        } else {
            port = static_cast<uint16_t>(atoi(argv[i]));
        }
    }
    FILE* dumpFile = nullptr;
    if (dumpPath) {
        dumpFile = fopen(dumpPath, "wb");
        if (!dumpFile) {
            fprintf(stderr, "[Host] Failed to open dump file: %s\n", dumpPath);
            return 1;
        }
        fprintf(stderr, "[Host] Dumping command stream to: %s\n", dumpPath);
    }

    HINSTANCE hInstance = GetModuleHandle(nullptr);

    // --- Create window ---
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "VBoxGPUBridgeServer";
    RegisterClassExA(&wc);

    RECT rect = { 0, 0, (LONG)WINDOW_WIDTH, (LONG)WINDOW_HEIGHT };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName,
                                "VBox GPU Bridge - Host Server",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                rect.right - rect.left, rect.bottom - rect.top,
                                nullptr, nullptr, hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);

    // --- Init Vulkan ---
    VulkanContext vk{};
    try {
        createInstance(vk);
        createSurface(vk, hwnd, hInstance);
        pickPhysicalDevice(vk);
        createLogicalDevice(vk);
    } catch (const std::exception& e) {
        fprintf(stderr, "Vulkan Init Error: %s\n", e.what());
        return 1;
    }

    VnDecoder decoder;
    decoder.init(vk.physicalDevice, vk.device, vk.graphicsQueue, vk.graphicsFamily, vk.surface);

    // --- Start TCP server ---
    TcpReceiver server;
    if (!server.listen(port)) {
        fprintf(stderr, "Failed to start TCP server on port %u\n", port);
        return 1;
    }
    if (!server.accept()) {
        fprintf(stderr, "Failed to accept connection\n");
        return 1;
    }

    fprintf(stderr, "[Host] Connection established. Processing command streams.\n");

    // --- Receive and process command streams ---
    constexpr size_t BUF_SIZE = 4 * 1024 * 1024; // 4 MB max per message
    std::vector<uint8_t> recvBuf(BUF_SIZE);

    while (g_running) {
        // Pump window messages
        MSG msg{};
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!g_running) break;

        // Check if data is available (non-blocking) before blocking recv
        if (!server.hasData(50)) continue; // 50ms poll, then pump messages again

        // Receive a command stream message
        size_t bytesRead = 0;
        if (!server.recv(recvBuf.data(), BUF_SIZE, bytesRead)) {
            fprintf(stderr, "[Host] Client disconnected.\n");
            break;
        }

        fprintf(stderr, "[Host] Received %zu bytes\n", bytesRead);

        // Dump to file if recording
        if (dumpFile) {
            uint32_t sz = static_cast<uint32_t>(bytesRead);
            fwrite(&sz, sizeof(sz), 1, dumpFile);
            fwrite(recvBuf.data(), 1, bytesRead, dumpFile);
            fflush(dumpFile);
        }

        // Execute the command stream
        if (!decoder.execute(recvBuf.data(), bytesRead)) {
            fprintf(stderr, "[Host] Command stream execution failed.\n");
            break;
        }

        // Find any swapchain (not hardcoded to ID 3)
        uint32_t imageIndex = 0;
        auto* sc = decoder.getFirstSwapchain();
        if (sc) imageIndex = sc->currentImageIndex;
        server.send(reinterpret_cast<const uint8_t*>(&imageIndex), sizeof(imageIndex));
    }

    // --- Cleanup ---
    if (dumpFile) fclose(dumpFile);
    decoder.cleanup();
    cleanupVulkan(vk);
    server.close();
    DestroyWindow(hwnd);
    UnregisterClassA(wc.lpszClassName, hInstance);
    fprintf(stderr, "[Host] Shutdown complete.\n");
    return 0;
}
