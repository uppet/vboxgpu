// Host TCP server: receives Venus command streams from Guest, decodes and renders.
// Also supports --replay <dump.bin> to replay a recorded command stream with screenshot.

#include "vn_decoder.h"
#include "win_capture.h"
#include "client_session.h"
#include "../../common/transport/tcp_transport.h"
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#include <lz4.h>
#include <cstdio>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstring>
#include <csignal>
#include <crtdbg.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

static void writeDump(EXCEPTION_POINTERS* ep) {
    CreateDirectoryA("S:\\bld\\vboxgpu\\dumps", NULL);
    HANDLE hFile = CreateFileA("S:\\bld\\vboxgpu\\dumps\\host_crash.dmp",
        GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
            (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithDataSegs | MiniDumpWithHandleData),
            ep ? &mei : NULL, NULL, NULL);
        CloseHandle(hFile);
        fprintf(stderr, "[Host] Crash dump: S:\\bld\\vboxgpu\\dumps\\host_crash.dmp\n");
        fflush(stderr);
    }
}

static LONG WINAPI hostCrashHandler(EXCEPTION_POINTERS* ep) {
    writeDump(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

// Catch abort() from assert failures — write dump without exception context
static void hostAbortHandler(int) {
    fprintf(stderr, "[Host] ABORT signal — writing crash dump\n"); fflush(stderr);
    writeDump(nullptr);
    _exit(99);
}

static constexpr uint32_t WINDOW_WIDTH = 400;
static constexpr uint32_t WINDOW_HEIGHT = 300;
static std::atomic<bool> g_running{true};

// Server dashboard state (updated by accept thread, read by WM_PAINT)
static uint16_t g_port = 0;
static int g_maxSessions = 0;
static std::vector<std::unique_ptr<ClientSession>>* g_sessions = nullptr;
static int g_totalConnections = 0;
static DWORD g_startTime = 0;
#define WM_REFRESH_DASHBOARD (WM_USER + 1)
#define IDC_HIDE_WINDOWS 101
static bool g_cloakWindows = false;

static void applyCloakToSessions(bool cloak) {
    g_cloakWindows = cloak;
    if (!g_sessions) return;
    BOOL val = cloak ? TRUE : FALSE;
    for (auto& s : *g_sessions) {
        if (s && s->hwnd())
            DwmSetWindowAttribute(s->hwnd(), DWMWA_CLOAK, &val, sizeof(val));
    }
}

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_startTime = GetTickCount();
        SetTimer(hwnd, 1, 500, nullptr);
        CreateWindowExA(0, "BUTTON", "Hide render windows",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            10, 260, 200, 24, hwnd, (HMENU)IDC_HIDE_WINDOWS,
            ((LPCREATESTRUCT)lParam)->hInstance, nullptr);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_HIDE_WINDOWS) {
            bool checked = (SendDlgItemMessageA(hwnd, IDC_HIDE_WINDOWS, BM_GETCHECK, 0, 0) == BST_CHECKED);
            applyCloakToSessions(checked);
        }
        return 0;
    case WM_TIMER:
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);

        // Background
        FillRect(hdc, &rc, (HBRUSH)(COLOR_WINDOW + 1));

        SetBkMode(hdc, TRANSPARENT);
        HFONT font = CreateFontA(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, "Consolas");
        HFONT fontBold = CreateFontA(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, "Consolas");
        SelectObject(hdc, fontBold);

        int y = 10;
        char buf[256];

        // Title
        snprintf(buf, sizeof(buf), "VBox GPU Bridge Server");
        TextOutA(hdc, 10, y, buf, (int)strlen(buf)); y += 24;

        SelectObject(hdc, font);

        // Uptime
        DWORD upSec = (GetTickCount() - g_startTime) / 1000;
        snprintf(buf, sizeof(buf), "Uptime: %um %02us    Port: %u",
                 upSec / 60, upSec % 60, g_port);
        TextOutA(hdc, 10, y, buf, (int)strlen(buf)); y += 20;

        // Session stats
        int active = 0;
        if (g_sessions) {
            for (auto& s : *g_sessions)
                if (s && s->isRunning()) active++;
        }
        snprintf(buf, sizeof(buf), "Sessions: %d/%d active    Total: %d",
                 active, g_maxSessions, g_totalConnections);
        TextOutA(hdc, 10, y, buf, (int)strlen(buf)); y += 24;

        // Per-session list
        if (g_sessions) {
            for (auto& s : *g_sessions) {
                if (!s) continue;
                snprintf(buf, sizeof(buf), "  [%d] %s%s",
                         s->id(),
                         s->isRunning() ? "ACTIVE" : "ended",
                         s->isReplay() ? " (replay)" : "");
                SetTextColor(hdc, s->isRunning() ? RGB(0, 128, 0) : RGB(128, 128, 128));
                TextOutA(hdc, 10, y, buf, (int)strlen(buf)); y += 18;
            }
        }
        SetTextColor(hdc, RGB(0, 0, 0));

        y += 10;
        snprintf(buf, sizeof(buf), "ESC or close window to shutdown");
        SetTextColor(hdc, RGB(128, 128, 128));
        TextOutA(hdc, 10, y, buf, (int)strlen(buf));

        DeleteObject(font);
        DeleteObject(fontBold);
        EndPaint(hwnd, &ps);
        return 0;
    }
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

static int replayMode(const char* dumpPath, const char* saveFramesDir = nullptr) {
    auto fileBatches = loadDumpFile(dumpPath);
    if (fileBatches.empty()) { fprintf(stderr, "[Replay] No batches.\n"); return 1; }

    // Convert to ClientSession::ReplayBatch
    std::vector<ClientSession::ReplayBatch> batches;
    batches.reserve(fileBatches.size());
    for (auto& fb : fileBatches)
        batches.push_back({std::move(fb.data)});

    HINSTANCE hInstance = GetModuleHandle(nullptr);

    // Shared Vulkan instance + physical device
    VulkanContext vk{};
    try {
        createInstance(vk);
        pickPhysicalDevice(vk);
    } catch (const std::exception& e) {
        fprintf(stderr, "Vulkan Init Error: %s\n", e.what());
        return 1;
    }

    // Create replay session via ClientSession
    ClientSession session(0, vk.physicalDevice, vk.instance, hInstance);
    session.startReplay(std::move(batches), saveFramesDir);

    // Wait for replay to finish
    session.join();

    cleanupVulkan(vk);
    fprintf(stderr, "[Replay] Done.\n");
    return 0;
}

int main(int argc, char* argv[]) {
    SetUnhandledExceptionFilter(hostCrashHandler);
    signal(SIGABRT, hostAbortHandler);
    // Suppress assert dialog in Debug builds — go straight to abort handler
    _set_abort_behavior(0, _WRITE_ABORT_MSG);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    setvbuf(stderr, NULL, _IONBF, 0);  // Force unbuffered stderr

    // Check --replay [--save-frames DIR] mode first
    const char* replayDump = nullptr;
    const char* saveFramesDir = nullptr;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--replay") == 0 && i + 1 < argc)
            replayDump = argv[++i];
        else if (strcmp(argv[i], "--save-frames") == 0 && i + 1 < argc)
            saveFramesDir = argv[++i];
    }
    if (replayDump)
        return replayMode(replayDump, saveFramesDir);

    uint16_t port = DEFAULT_PORT;
    const char* dumpDir = nullptr;  // --dump DIR: each session writes session_N.bin
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
            dumpDir = argv[++i];
        } else {
            port = static_cast<uint16_t>(atoi(argv[i]));
        }
    }
    if (dumpDir) {
        CreateDirectoryA(dumpDir, NULL);
        fprintf(stderr, "[Host] Recording sessions to: %s\\session_*.bin\n", dumpDir);
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

    // --- Quick test (DISABLED to avoid surface reuse issues): ---
#if 0 can the swapchain display ANYTHING? ---
    {
        // Create a minimal swapchain on the Host surface
        VkSurfaceCapabilitiesKHR caps;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.physicalDevice, vk.surface, &caps);
        uint32_t fmtCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physicalDevice, vk.surface, &fmtCount, nullptr);
        std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physicalDevice, vk.surface, &fmtCount, fmts.data());

        VkSwapchainCreateInfoKHR sci{};
        sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        sci.surface = vk.surface;
        sci.minImageCount = 2;
        sci.imageFormat = fmts[0].format;
        sci.imageColorSpace = fmts[0].colorSpace;
        sci.imageExtent = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : VkExtent2D{800,600};
        sci.imageArrayLayers = 1;
        sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        sci.preTransform = caps.currentTransform;
        sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        sci.clipped = VK_TRUE;

        VkSwapchainKHR testSc;
        VkResult scr = vkCreateSwapchainKHR(vk.device, &sci, nullptr, &testSc);
        fprintf(stderr, "[SelfTest] Swapchain create: result=%d fmt=%u extent=%ux%u\n",
                (int)scr, fmts[0].format, sci.imageExtent.width, sci.imageExtent.height);

        if (scr == VK_SUCCESS) {
            uint32_t imgCount = 0;
            vkGetSwapchainImagesKHR(vk.device, testSc, &imgCount, nullptr);
            std::vector<VkImage> imgs(imgCount);
            vkGetSwapchainImagesKHR(vk.device, testSc, &imgCount, imgs.data());

            // Present RED on each image a few times
            VkCommandPool pool;
            VkCommandPoolCreateInfo pci{}; pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pci.queueFamilyIndex = vk.graphicsFamily;
            vkCreateCommandPool(vk.device, &pci, nullptr, &pool);

            VkFence fence;
            VkFenceCreateInfo fci{}; fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            vkCreateFence(vk.device, &fci, nullptr, &fence);

            for (int frame = 0; frame < 10; frame++) {
                uint32_t idx;
                vkAcquireNextImageKHR(vk.device, testSc, UINT64_MAX, VK_NULL_HANDLE, fence, &idx);
                vkWaitForFences(vk.device, 1, &fence, VK_TRUE, UINT64_MAX);
                vkResetFences(vk.device, 1, &fence);

                VkCommandBuffer cb;
                VkCommandBufferAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
                vkAllocateCommandBuffers(vk.device, &ai, &cb);
                VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkBeginCommandBuffer(cb, &bi);

                VkImageMemoryBarrier b{}; b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                b.image = imgs[idx]; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,0,nullptr,0,nullptr,1,&b);

                VkClearColorValue red = {{1,0,0,1}};
                VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
                vkCmdClearColorImage(cb, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &red, 1, &range);

                b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,0,nullptr,0,nullptr,1,&b);

                vkEndCommandBuffer(cb);
                VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
                vkQueueSubmit(vk.graphicsQueue, 1, &si, VK_NULL_HANDLE);
                vkQueueWaitIdle(vk.graphicsQueue);

                VkPresentInfoKHR pi{}; pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
                pi.swapchainCount = 1; pi.pSwapchains = &testSc; pi.pImageIndices = &idx;
                VkResult pr = vkQueuePresentKHR(vk.graphicsQueue, &pi);
                if (frame == 0)
                    fprintf(stderr, "[SelfTest] Present frame %d imgIdx=%u result=%d\n", frame, idx, (int)pr);

                vkFreeCommandBuffers(vk.device, pool, 1, &cb);
                MSG msg{}; while (PeekMessageA(&msg,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
                Sleep(30);
            }

            // WGC capture after self-test
            captureWindowToBMP(hwnd, "S:/bld/vboxgpu/selftest_capture.bmp");

            vkDestroyFence(vk.device, fence, nullptr);
            vkDestroyCommandPool(vk.device, pool, nullptr);
            vkDestroySwapchainKHR(vk.device, testSc, nullptr);
        }
    }
#endif

    // --- Multi-client accept loop ---
    int maxSessions = DEFAULT_MAX_SESSIONS;
    const char* maxStr = getenv("VBOXGPU_MAX_SESSIONS");
    if (maxStr) maxSessions = atoi(maxStr);
    if (maxSessions < 1) maxSessions = 1;
    fprintf(stderr, "[Host] Max concurrent sessions: %d\n", maxSessions);

    // TCP listen socket (shared across all sessions)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int reuseAddr = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuseAddr), sizeof(reuseAddr));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (::bind(listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        fprintf(stderr, "[Host] Bind failed: %d\n", WSAGetLastError());
        return 1;
    }
    ::listen(listenSock, maxSessions);
    fprintf(stderr, "[TcpReceiver] Listening on port %u...\n", port);

    // Session pool
    std::vector<std::unique_ptr<ClientSession>> sessions;
    int nextSessionId = 0;
    g_sessions = &sessions;
    g_port = port;
    g_maxSessions = maxSessions;

    // --- Accept loop: spawn ClientSession per connection ---
    // Accept runs on a dedicated thread; main thread pumps window messages.
    std::thread acceptThread([&]() {
        while (g_running) {
            // Reap finished sessions
            sessions.erase(
                std::remove_if(sessions.begin(), sessions.end(),
                    [](auto& s) { return !s->isRunning(); }),
                sessions.end());

            if ((int)sessions.size() >= maxSessions) {
                Sleep(100); // at capacity, poll for slot
                continue;
            }

            SOCKET cs = ::accept(listenSock, nullptr, nullptr);
            if (cs == INVALID_SOCKET) {
                if (g_running) fprintf(stderr, "[Host] accept() error: %d\n", WSAGetLastError());
                break;
            }
            // Match client-side socket settings
            int flag = 1;
            setsockopt(cs, IPPROTO_TCP, TCP_NODELAY,
                       reinterpret_cast<const char*>(&flag), sizeof(flag));
            int bufsz = 4 * 1024 * 1024;
            setsockopt(cs, SOL_SOCKET, SO_SNDBUF,
                       reinterpret_cast<const char*>(&bufsz), sizeof(bufsz));
            setsockopt(cs, SOL_SOCKET, SO_RCVBUF,
                       reinterpret_cast<const char*>(&bufsz), sizeof(bufsz));

            int sid = nextSessionId++;
            g_totalConnections = sid + 1;
            fprintf(stderr, "[Host] Client connected → Session %d (active: %d/%d)\n",
                    sid, (int)sessions.size() + 1, maxSessions);

            auto session = std::make_unique<ClientSession>(
                sid, vk.physicalDevice, vk.instance, hInstance);
            session->start(cs, dumpDir);
            // Apply cloak if checkbox is checked
            if (g_cloakWindows && session->hwnd()) {
                // Small delay — window created on worker thread, may not exist yet
                Sleep(100);
                if (session->hwnd()) {
                    BOOL val = TRUE;
                    DwmSetWindowAttribute(session->hwnd(), DWMWA_CLOAK, &val, sizeof(val));
                }
            }
            sessions.push_back(std::move(session));
        }
    });

    // --- Main thread: window message loop ---
    while (g_running) {
        MSG msg{};
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!g_running) break;
        Sleep(10);
    }

    // --- Cleanup ---
    g_running = false;
    closesocket(listenSock); // unblock accept()
    if (acceptThread.joinable()) acceptThread.join();
    sessions.clear(); // destructor joins threads + cleans up
    cleanupVulkan(vk);
    DestroyWindow(hwnd);
    UnregisterClassA(wc.lpszClassName, hInstance);
    WSACleanup();
    fprintf(stderr, "[Host] Shutdown complete.\n");
    return 0;
}
