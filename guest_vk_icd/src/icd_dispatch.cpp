// VBox GPU Bridge — Vulkan ICD dispatch table.
// Maps Vulkan function names to our proxy implementations.

// Performance switches — sync with host vn_decoder.h
#ifndef VBOXGPU_PERF_DIRTY_TRACK
#define VBOXGPU_PERF_DIRTY_TRACK 1
#endif

#include "icd_dispatch.h"
#include "../../common/vboxgpu_config.h"
#include <lz4.h>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <string>
#include <cstddef>
#include <stdexcept>

// MEM_WRITE_WATCH dirty tracking: no VEH handler needed.
// The kernel tracks writes at page granularity; GetWriteWatch atomically
// returns dirty pages and resets tracking — zero race window.

IcdState g_icd;

// Quick debug log to file via Win32 API (static CRT fopen unreliable)
static void icdDbg(const char* msg) {
    HANDLE h = CreateFileA("S:\\bld\\vboxgpu\\icd_debug.log",
        FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(h, msg, (DWORD)strlen(msg), &written, NULL);
        WriteFile(h, "\r\n", 2, &written, NULL);
        CloseHandle(h);
    }
}

// Lock guard for encoder — DXVK is multithreaded, encoder is not thread-safe
// All encoder access uses encoder.mutex_ — unified single mutex.
// Uses recursive_mutex so QueueSubmit can hold the lock for the full
// flush+submit sequence while internal encoder methods also lock it.
#define ENC_LOCK std::lock_guard<std::recursive_mutex> _enc_lock(g_icd.encoder.mutex_)

// --- Crash dump generation ---
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

static LONG WINAPI crashDumpHandler(EXCEPTION_POINTERS* ep) {
    // Write to icd_crash.dmp (distinct from host dump)
    HANDLE hFile = CreateFileA("S:\\bld\\vboxgpu\\dumps\\icd_crash.dmp",
        GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
            (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithDataSegs),
            &mei, NULL, NULL);
        CloseHandle(hFile);
    }
    // Also log exception code to debug log
    char buf[256];
    wsprintfA(buf, "[ICD] CRASH: ExceptionCode=0x%08X", ep ? ep->ExceptionRecord->ExceptionCode : 0);
    icdDbg(buf);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void writeDumpIcd(EXCEPTION_POINTERS* ep) {
    CreateDirectoryA("S:\\bld\\vboxgpu\\dumps", NULL);
    HANDLE hFile = CreateFileA("S:\\bld\\vboxgpu\\dumps\\icd_crash.dmp",
        GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        if (ep) { mei.ThreadId = GetCurrentThreadId(); mei.ExceptionPointers = ep; mei.ClientPointers = FALSE; }
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
            (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithDataSegs),
            ep ? &mei : NULL, NULL, NULL);
        CloseHandle(hFile);
    }
}

static void icdTerminateHandler() {
    icdDbg("[ICD] TERMINATE called (uncaught C++ exception?)");
    try { throw; } catch (const std::exception& e) {
        std::string msg = "[ICD] exception: "; msg += e.what();
        icdDbg(msg.c_str());
    } catch (...) {
        icdDbg("[ICD] unknown exception");
    }
    writeDumpIcd(nullptr);
    _exit(42);
}

struct CrashHandlerInstaller {
    CrashHandlerInstaller() {
        CreateDirectoryA("S:\\bld\\vboxgpu\\dumps", NULL);
        SetUnhandledExceptionFilter(crashDumpHandler);
        std::set_terminate(icdTerminateHandler);
        icdDbg("ICD DLL loaded, crash handler installed");
    }
} g_crashHandler;

// Dispatchable handles (VkInstance, VkDevice, VkQueue, VkCommandBuffer)
// The Vulkan loader wraps our handles in its own trampoline objects,
// so we CANNOT embed our ID inside the handle struct.
// Instead we maintain a global pointer→ID mapping.
struct DispatchableHandle {
    void* loaderDisp; // loader writes its dispatch table here (MUST be first)
    uint64_t id;
};

static uint64_t toId(void* handle) {
    if (!handle) return 0;
    return reinterpret_cast<DispatchableHandle*>(handle)->id;
}

// Vulkan loader requires this magic value as the first dword of dispatchable handles (Debug builds assert on it)
#define ICD_LOADER_MAGIC 0x01CDC0DE

static void* makeDispatchable(uint64_t id) {
    auto* h = new DispatchableHandle();
    *reinterpret_cast<uintptr_t*>(h) = ICD_LOADER_MAGIC;
    h->id = id;
    return h;
}

// Non-dispatchable handles: cast uint64_t directly
static uint64_t ndToId(uint64_t handle) { return handle; }
static uint64_t idToNd(uint64_t id) { return id; }

// Forward declaration
static PFN_vkVoidFunction lookupFunc(const char* pName);
static uint32_t VKAPI_CALL icd_vkGetImageViewHandleNVX(VkDevice, const void*);
static VkResult VKAPI_CALL icd_vkGetImageViewAddressNVX(VkDevice, VkImageView, void*);

// --- ICD State ---

bool IcdState::connectToHost(const char* host, uint16_t port) {
    if (!transport.connect(host, port)) return false;
    memoryShadows.reserve(256);
    connected = true;
    startRecvThread();
    return true;
}

void IcdState::flushBufferRange(uint64_t bufferId, VkDeviceSize offset, VkDeviceSize range) {
    auto bit = bufferBindings.find(bufferId);
    if (bit == bufferBindings.end()) {
#ifdef VBOXGPU_VERBOSE
        static int miss = 0;
        if (miss++ < 10) icdDbg(("[ICD] flushBufRange MISS: buf=" + std::to_string(bufferId) + " NOT in bufferBindings").c_str());
#endif
        return;
    }
    uint64_t memId = bit->second.memoryId;
    VkDeviceSize memBase = bit->second.memoryOffset;
    std::lock_guard<std::mutex> lock(mappedMutex);
    for (auto& m : mappedRegions) {
        if (m.memoryId != memId) continue;
        VkDeviceSize dataStart = memBase + offset;
        VkDeviceSize dataEnd = (range == VK_WHOLE_SIZE) ? m.offset + m.size : dataStart + range;
        if (dataStart >= m.offset && dataStart < m.offset + m.size) {
            VkDeviceSize localOff = dataStart - m.offset;
            uint32_t sz = (uint32_t)(dataEnd - dataStart);
            if (sz > m.size - localOff) sz = (uint32_t)(m.size - localOff);
#ifdef VBOXGPU_VERBOSE
            static int flushOk = 0;
            if (flushOk++ < 10 || sz > 100000)
                icdDbg(("[ICD] flushBufRange OK: buf=" + std::to_string(bufferId) + " mem=" + std::to_string(memId) + " off=" + std::to_string(dataStart) + " sz=" + std::to_string(sz)).c_str());
#endif
            encoder.cmdWriteMemory(memId, dataStart, sz,
                                   (const uint8_t*)m.ptr + localOff);
            return;
        }
    }
#ifdef VBOXGPU_VERBOSE
    // Fell through — buffer binding found but no matching mapped region
    static int mapMiss = 0;
    if (mapMiss++ < 10)
        icdDbg(("[ICD] flushBufRange MAP MISS: buf=" + std::to_string(bufferId)
                + " mem=" + std::to_string(memId) + " off=" + std::to_string(offset)
                + " range=" + std::to_string(range) + " mappedCount=" + std::to_string(mappedRegions.size())).c_str());
#endif
}

void IcdState::flushMappedMemory() {
    std::lock_guard<std::mutex> lock(mappedMutex);
#if VBOXGPU_PERF_DIRTY_TRACK
    // (freed shadows are now cleaned up immediately in icd_vkFreeMemory)

    // Phase 1: GetWriteWatch per shadow — atomically get dirty pages + reset tracking.
    // No race window: kernel does get+reset in one operation.
    struct ShadowDirty {
        std::vector<uintptr_t> offsets; // page-aligned byte offsets from shadow base
    };
    std::unordered_map<uint64_t, ShadowDirty> dirtyMap;

    for (auto& [memId, shadow] : memoryShadows) {
        if (!shadow.ptr) continue;

        // Large shadows: reset dirty bits to prevent stale accumulation,
        // but do not send data (handled by explicit flushBufferRange).
        if (shadow.size > VBOXGPU_DIRTY_TRACK_SIZE_LIMIT) {
            ResetWriteWatch(shadow.ptr, (SIZE_T)shadow.size);
            continue;
        }

        ULONG_PTR maxPages = (ULONG_PTR)((shadow.size + 4095) / 4096);
        std::vector<void*> pages(maxPages);
        ULONG_PTR count = maxPages;
        ULONG granularity = 0;

        UINT res = GetWriteWatch(WRITE_WATCH_FLAG_RESET,
                                  shadow.ptr, (SIZE_T)shadow.size,
                                  pages.data(), &count, &granularity);
        if (res != 0 || count == 0) continue;

        auto& info = dirtyMap[memId];
        info.offsets.reserve(count);
        uintptr_t base = (uintptr_t)shadow.ptr;
        for (ULONG_PTR i = 0; i < count; i++)
            info.offsets.push_back((uintptr_t)pages[i] - base);
        // offsets are sorted ascending (guaranteed by GetWriteWatch)
    }

    // Phase 2: For each mapped region, find overlapping dirty pages → merge runs → send.
    for (auto& m : mappedRegions) {
        if (m.size > VBOXGPU_DIRTY_TRACK_SIZE_LIMIT) continue;
        auto dit = dirtyMap.find(m.memoryId);
        if (dit == dirtyMap.end()) continue;
        auto sit = memoryShadows.find(m.memoryId);
        if (sit == memoryShadows.end()) continue;
        auto& shadow = sit->second;

        VkDeviceSize regStart = m.offset;
        VkDeviceSize regEnd = m.offset + m.size;

        // Merge contiguous dirty pages into runs for efficient WriteMemory
        VkDeviceSize runStart = VK_WHOLE_SIZE, runEnd = 0;

        for (auto off : dit->second.offsets) {
            VkDeviceSize pageStart = (VkDeviceSize)off;
            VkDeviceSize pageEnd = pageStart + 4096;

            // Skip pages outside this mapped region
            if (pageEnd <= regStart || pageStart >= regEnd) continue;

            // Clip to region bounds
            VkDeviceSize clipStart = pageStart < regStart ? regStart : pageStart;
            VkDeviceSize clipEnd = pageEnd > regEnd ? regEnd : pageEnd;

            if (runStart == VK_WHOLE_SIZE) {
                runStart = clipStart;
                runEnd = clipEnd;
            } else if (clipStart <= runEnd) {
                // Extend current run
                if (clipEnd > runEnd) runEnd = clipEnd;
            } else {
                // Emit previous run
                encoder.cmdWriteMemory(m.memoryId, runStart, (uint32_t)(runEnd - runStart),
                                       (const uint8_t*)shadow.ptr + runStart);
                runStart = clipStart;
                runEnd = clipEnd;
            }
        }
        // Emit final run
        if (runStart != VK_WHOLE_SIZE) {
            encoder.cmdWriteMemory(m.memoryId, runStart, (uint32_t)(runEnd - runStart),
                                   (const uint8_t*)shadow.ptr + runStart);
        }
    }
#else
    for (auto& m : mappedRegions) {
        if (m.size <= VBOXGPU_DIRTY_TRACK_SIZE_LIMIT) {
            encoder.cmdWriteMemory(m.memoryId, m.offset, (uint32_t)m.size, m.ptr);
        }
    }
#endif
}

bool IcdState::sendBatchLocked(bool isPresent) {
    // Caller MUST hold encoder.mutex_
    uint32_t seqId = nextSeqId_++;
#if VBOXGPU_TIMING
    uint64_t t0 = rtNowUs();
    encoder.cmdBridgeTimingSeqUnlocked(seqId, t0);
    RT_LOG(seqId, "T1", "sendBatch %zu bytes present=%d", encoder.size(), (int)isPresent);
#endif
    encoder.cmdEndOfStreamUnlocked();
    size_t sendSize = encoder.size();
    bool ok = transport.send(encoder.data(), sendSize);
    encoder.w_ = VnStreamWriter();
    if (!ok) return false;
    // Record response type — atomic with TCP send under encoder.mutex_
    {
        std::lock_guard<std::mutex> lock(pendingQueueMutex_);
        pendingResponseQueue_.push({isPresent, seqId, rtNowUs()});
    }
    return true;
}

bool IcdState::sendBatch(bool isPresent) {
    std::lock_guard<std::recursive_mutex> lock(encoder.mutex_);
    return sendBatchLocked(isPresent);
}

void IcdState::startRecvThread() {
    recvRunning_ = true;
    recvThread_ = std::thread([this]{ recvLoop(); });
}

void IcdState::stopRecvThread() {
    recvRunning_ = false;
    transport.close(); // unblocks any pending recv
    if (recvThread_.joinable()) recvThread_.join();
}

void IcdState::recvLoop() {
    constexpr size_t RECV_BUF_SIZE = 8 * 1024 * 1024;
    std::vector<uint8_t> recvBuf(RECV_BUF_SIZE);

    while (recvRunning_) {
        size_t n = transport.recv(recvBuf.data(), RECV_BUF_SIZE);
        if (n == 0) break; // disconnected

        // Determine response type from the ordered queue
        bool isPresent = true;
        uint32_t batchSeqId = 0;
        uint64_t batchSendTs = 0;
        {
            std::lock_guard<std::mutex> lock(pendingQueueMutex_);
            if (!pendingResponseQueue_.empty()) {
                auto& front = pendingResponseQueue_.front();
                isPresent = front.isPresent;
                batchSeqId = front.seqId;
                batchSendTs = front.sendTimestampUs;
                pendingResponseQueue_.pop();
            }
        }
#if VBOXGPU_TIMING
        uint64_t tRecv = rtNowUs();
        RT_LOG(batchSeqId, "T7", "recv %zu bytes, wait=%.2fms",
               n, (tRecv - batchSendTs) / 1000.0);
#endif

        // Parse imageIndex
        uint32_t imageIndex = 0;
        if (n >= 4) memcpy(&imageIndex, recvBuf.data(), 4);

        // Parse frame: [imageIndex(4)][w(4)][h(4)][compressedSize(4)][LZ4 data...]
        uint32_t compressedSz = 0;
        if (n >= 16) {
            uint32_t w, h;
            memcpy(&w,            recvBuf.data() + 4,  4);
            memcpy(&h,            recvBuf.data() + 8,  4);
            memcpy(&compressedSz, recvBuf.data() + 12, 4);
            if (compressedSz > 0 && n >= 16 + compressedSz) {
                uint32_t rawSize = w * h * 4;
                std::vector<uint8_t> pixels(rawSize);
#if VBOXGPU_TIMING
                uint64_t tDecompStart = rtNowUs();
#endif
                int dec = LZ4_decompress_safe(
                    reinterpret_cast<const char*>(recvBuf.data() + 16),
                    reinterpret_cast<char*>(pixels.data()),
                    compressedSz, rawSize);
                if (dec == (int)rawSize) {
#if VBOXGPU_TIMING
                    RT_LOG(batchSeqId, "T8", "decompress %.2fms (%u->%u)",
                           (rtNowUs() - tDecompStart) / 1000.0, compressedSz, rawSize);
#endif
                    framePixels = std::move(pixels);
                    frameWidth = w;
                    frameHeight = h;
                    frameValid = true;
                    blitFrameToWindow(); // rate-limited to ~60 FPS
#if VBOXGPU_TIMING
                    RT_LOG(batchSeqId, "T9", "roundtrip=%.2fms",
                           (rtNowUs() - batchSendTs) / 1000.0);
#endif
                }
            }
        }

        // Parse BDA results suffix: [bdaCount(4)][{bufId(8),addr(8)}*N]
        size_t bdaOff = 16 + compressedSz;
        uint32_t bdaCount = 0;
        if (n >= bdaOff + 4) {
            memcpy(&bdaCount, recvBuf.data() + bdaOff, 4);
            if (bdaCount > 0) {
                std::lock_guard<std::mutex> lock(bdaMutex_);
                for (uint32_t i = 0; i < bdaCount && bdaOff + 4 + (i + 1) * 16 <= n; i++) {
                    uint64_t rBufId = 0, rAddr = 0;
                    memcpy(&rBufId, recvBuf.data() + bdaOff + 4 + i * 16,     8);
                    memcpy(&rAddr,  recvBuf.data() + bdaOff + 4 + i * 16 + 8, 8);
                    bdaCache[rBufId] = rAddr;
                }
                bdaCV_.notify_all();
            }
        }

#if VBOXGPU_TIMING
        // Parse timing suffix: batch(20) + frame(28) = 48 bytes
        size_t timingOff = bdaOff + 4 + bdaCount * 16;
        if (n >= timingOff + 48) {
            // Batch timing
            uint64_t hostRecvUs = 0, hostSendUs = 0;
            memcpy(&hostRecvUs, recvBuf.data() + timingOff + 4,  8);
            memcpy(&hostSendUs, recvBuf.data() + timingOff + 12, 8);
            double hostMs = (hostSendUs - hostRecvUs) / 1000.0;
            double netMs = (rtNowUs() - batchSendTs) / 1000.0 - hostMs;
            RT_LOG(batchSeqId, "NET", "host=%.2fms net=%.2fms", hostMs, netMs);

            // Frame timing: end-to-end pipeline latency
            uint32_t ftFrameId = 0;
            uint64_t ftPresentUs = 0, ftReadbackUs = 0, ftCompressUs = 0;
            memcpy(&ftFrameId,    recvBuf.data() + timingOff + 20, 4);
            memcpy(&ftPresentUs,  recvBuf.data() + timingOff + 24, 8);
            memcpy(&ftReadbackUs, recvBuf.data() + timingOff + 32, 8);
            memcpy(&ftCompressUs, recvBuf.data() + timingOff + 40, 8);
            if (ftFrameId > 0 && ftPresentUs > 0) {
                // All host timestamps are relative to host process start.
                // We can compute host-side durations, but not cross-machine absolute.
                // Use hostSendUs - ftPresentUs as the host pipeline age.
                double hostPipeMs = (hostSendUs - ftPresentUs) / 1000.0;
                double readbackMs = (ftReadbackUs - ftPresentUs) / 1000.0;
                double compressMs = (ftCompressUs - ftReadbackUs) / 1000.0;
                double sendMs = (hostSendUs - ftCompressUs) / 1000.0;
                // Guest-side: recv + decompress + blit already logged as T7/T8/T9
                // Estimate total pipeline: host_pipe + net_one_way + guest_decomp_blit
                // net_one_way ≈ netMs/2 (rough, symmetric assumption)
                RT_LOG(ftFrameId, "FT",
                       "pipeline: readback=%.2f compress=%.2f send=%.2f host_total=%.2fms",
                       readbackMs, compressMs, sendMs, hostPipeMs);
            }
        }
#endif

        // Signal the appropriate waiter
        if (isPresent) {
            std::lock_guard<std::mutex> lock(acquireMutex_);
            currentImageIndex = imageIndex;
            imageIndexReady_ = true;
            acquireCV_.notify_one();
        } else {
            // BDA query response: bdaCV_ already notified above if results present
        }
    }
}

uint64_t IcdState::syncGetBufferDeviceAddress(uint64_t bufferId) {
    // Fast path: cache hit (auto-BDA from BindBufferMemory response)
    {
        std::lock_guard<std::mutex> lock(bdaMutex_);
        auto it = bdaCache.find(bufferId);
        if (it != bdaCache.end()) return it->second;
    }

    // Cache miss — BindBufferMemory already flushed this buffer to Host.
    // Host auto-BDA generates the address in the response.
    // Just wait for the recv thread to deliver it.
    uint64_t result = 0;
    {
        std::unique_lock<std::mutex> lock(bdaMutex_);
        bdaCV_.wait_for(lock, std::chrono::seconds(5),
            [this, bufferId]{ return bdaCache.count(bufferId) > 0; });
        auto it = bdaCache.find(bufferId);
        if (it != bdaCache.end()) result = it->second;
    }
    return result;
}

void IcdState::blitFrameToWindow() {
    if (!frameValid || !presentHwnd || framePixels.empty()) return;

    // Rate-limit GDI blit to ~60 FPS — prevents VM CPU saturation when
    // host renders 1000+ FPS via async LZ4. Game loop stays fast; only
    // display output is capped.
    auto now = std::chrono::steady_clock::now();
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         now - lastBlitTime).count();
    if (elapsedMs < BLIT_INTERVAL_MS) return;
    lastBlitTime = now;

    HDC hdc = GetDC(presentHwnd);
    if (!hdc) return;

    // BITMAPINFO for top-down BGRA (negative height = top-down)
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(frameWidth);
    bmi.bmiHeader.biHeight = -static_cast<LONG>(frameHeight); // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    // Get client rect for potential stretching
    RECT rc;
    GetClientRect(presentHwnd, &rc);
    int dstW = rc.right - rc.left;
    int dstH = rc.bottom - rc.top;

    if (dstW == (int)frameWidth && dstH == (int)frameHeight) {
        // Exact match: fast path
        SetDIBitsToDevice(hdc, 0, 0, frameWidth, frameHeight,
                          0, 0, 0, frameHeight,
                          framePixels.data(), &bmi, DIB_RGB_COLORS);
    } else {
        // Stretch to fit
        SetStretchBltMode(hdc, COLORONCOLOR);
        StretchDIBits(hdc, 0, 0, dstW, dstH,
                      0, 0, frameWidth, frameHeight,
                      framePixels.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
    }

    ReleaseDC(presentHwnd, hdc);
}

// =============================================================
// Vulkan function implementations
// =============================================================

// --- Instance ---
static VkResult VKAPI_CALL icd_vkCreateInstance(
    const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance* pInstance)
{
    icdDbg("vkCreateInstance: enter");
    g_icd.initDefaults();
    icdDbg("vkCreateInstance: initDefaults done");
    uint64_t id = g_icd.handles.alloc();
    *pInstance = reinterpret_cast<VkInstance>(makeDispatchable(id));
    icdDbg("vkCreateInstance: done");
    return VK_SUCCESS;
}

static void VKAPI_CALL icd_vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks*) {
    if (instance) delete reinterpret_cast<DispatchableHandle*>(instance);
}

static VkResult VKAPI_CALL icd_vkEnumeratePhysicalDevices(
    VkInstance, uint32_t* pCount, VkPhysicalDevice* pDevices)
{
    fprintf(stderr, "[ICD] vkEnumeratePhysicalDevices: pDevices=%p count=%u\n",
            (void*)pDevices, pCount ? *pCount : 0);
    fflush(stderr);
    if (!pDevices) { *pCount = 1; return VK_SUCCESS; }
    if (*pCount < 1) { *pCount = 1; return VK_INCOMPLETE; }
    static DispatchableHandle physDev = { nullptr, 1 };
    pDevices[0] = reinterpret_cast<VkPhysicalDevice>(&physDev);
    *pCount = 1;
    return VK_SUCCESS;
}

// --- Physical device queries ---
static void VKAPI_CALL icd_vkGetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties* p) {
    fprintf(stderr, "[ICD] vkGetPhysicalDeviceProperties\n"); fflush(stderr);
    *p = g_icd.physDeviceProps;
}

static void VKAPI_CALL icd_vkGetPhysicalDeviceProperties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2* p) {
    fprintf(stderr, "[ICD] vkGetPhysicalDeviceProperties2\n"); fflush(stderr);
    icd_vkGetPhysicalDeviceProperties(pd, &p->properties);
    // Carefully fill only the fields DXVK absolutely needs — leave everything else zero.
    VkBaseOutStructure* next = reinterpret_cast<VkBaseOutStructure*>(p->pNext);
    while (next) {
        switch (next->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES: {
            auto* p11 = reinterpret_cast<VkPhysicalDeviceVulkan11Properties*>(next);
            p11->subgroupSize = 32;
            p11->subgroupSupportedStages = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
            p11->subgroupSupportedOperations = 0xFF; // all basic ops
            p11->subgroupQuadOperationsInAllStages = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES: {
            auto* p12 = reinterpret_cast<VkPhysicalDeviceVulkan12Properties*>(next);
            p12->driverID = VK_DRIVER_ID_NVIDIA_PROPRIETARY;
            strncpy(p12->driverName, "VBox GPU Bridge", VK_MAX_DRIVER_NAME_SIZE);
            p12->maxTimelineSemaphoreValueDifference = UINT64_MAX;
            p12->maxUpdateAfterBindDescriptorsInAllPools = 1048576;
            p12->maxPerStageDescriptorUpdateAfterBindSamplers = 1048576;
            p12->maxPerStageDescriptorUpdateAfterBindUniformBuffers = 15;
            p12->maxPerStageDescriptorUpdateAfterBindStorageBuffers = 1048576;
            p12->maxPerStageDescriptorUpdateAfterBindSampledImages = 1048576;
            p12->maxPerStageDescriptorUpdateAfterBindStorageImages = 1048576;
            p12->maxPerStageDescriptorUpdateAfterBindInputAttachments = 1048576;
            p12->maxPerStageUpdateAfterBindResources = 1048576;
            p12->maxDescriptorSetUpdateAfterBindSamplers = 1048576;
            p12->maxDescriptorSetUpdateAfterBindUniformBuffers = 90;
            p12->maxDescriptorSetUpdateAfterBindStorageBuffers = 1048576;
            p12->maxDescriptorSetUpdateAfterBindSampledImages = 1048576;
            p12->maxDescriptorSetUpdateAfterBindStorageImages = 1048576;
            p12->maxDescriptorSetUpdateAfterBindInputAttachments = 256;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES: {
            auto* p13 = reinterpret_cast<VkPhysicalDeviceVulkan13Properties*>(next);
            p13->minSubgroupSize = 32;
            p13->maxSubgroupSize = 32;
            p13->maxComputeWorkgroupSubgroups = 32;
            p13->maxInlineUniformBlockSize = 256;
            p13->maxPerStageDescriptorInlineUniformBlocks = 4;
            p13->maxDescriptorSetInlineUniformBlocks = 4;
            p13->maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks = 4;
            p13->maxDescriptorSetUpdateAfterBindInlineUniformBlocks = 4;
            p13->maxInlineUniformTotalSize = 4096;
            p13->maxBufferSize = 2ull * 1024 * 1024 * 1024;
            break;
        }
        default:
            // Leave unknown pNext structs untouched (zero-initialized by DXVK)
            break;
        }
        next = next->pNext;
    }
    fprintf(stderr, "[ICD] vkGetPhysicalDeviceProperties2 DONE\n"); fflush(stderr);
}

static void VKAPI_CALL icd_vkGetPhysicalDeviceFeatures(VkPhysicalDevice, VkPhysicalDeviceFeatures* p) {
    *p = g_icd.physDeviceFeatures;
}

static void VKAPI_CALL icd_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2* p) {
    fprintf(stderr, "[ICD] vkGetPhysicalDeviceFeatures2\n"); fflush(stderr);
    icd_vkGetPhysicalDeviceFeatures(pd, &p->features);
    // Walk pNext and enable all boolean features in known structs
    VkBaseOutStructure* next = reinterpret_cast<VkBaseOutStructure*>(p->pNext);
    while (next) {
        // Set all VkBool32 fields to VK_TRUE for feature structs.
        // Feature structs have sType + pNext + VkBool32 fields.
        // We memset the fields after the header to VK_TRUE.
        // Enable all boolean features for known feature structs.
        // CAREFUL: only fill fields we know exist — don't overwrite past struct end.
        VkBool32* bools = reinterpret_cast<VkBool32*>(
            reinterpret_cast<uint8_t*>(next) + sizeof(VkBaseOutStructure));

        // Map sType → number of VkBool32 fields after the header
        size_t numBools = 0;
        // Use compile-time sizeof to determine exact bool count per struct.
        // All feature structs: { sType(4) + pad(4) + pNext(8) + VkBool32 fields... }
        // Use compile-time sizeof for exact VkBool32 count — prevents pNext corruption
        #define FB(stype, ctype) case stype: \
            numBools = (sizeof(ctype) - sizeof(VkBaseOutStructure)) / sizeof(VkBool32); \
            fprintf(stderr, "[ICD]   features2 sType=%u → %zu bools\n", (unsigned)stype, numBools); \
            break
        switch (next->sType) {
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, VkPhysicalDeviceVulkan11Features);
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, VkPhysicalDeviceVulkan12Features);
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, VkPhysicalDeviceVulkan13Features);
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, VkPhysicalDeviceShaderDrawParametersFeatures);
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES, VkPhysicalDeviceDescriptorIndexingFeatures);
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES, VkPhysicalDeviceHostQueryResetFeatures);
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, VkPhysicalDeviceTimelineSemaphoreFeatures);
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, VkPhysicalDeviceBufferDeviceAddressFeatures);
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, VkPhysicalDeviceDynamicRenderingFeatures);
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR, VkPhysicalDeviceSynchronization2Features);
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES, VkPhysicalDeviceMaintenance4Features);
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES, VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures);
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES, VkPhysicalDevicePrivateDataFeatures);
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES, VkPhysicalDevicePipelineCreationCacheControlFeatures);
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES, VkPhysicalDeviceInlineUniformBlockFeatures);
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES, VkPhysicalDeviceSubgroupSizeControlFeatures);
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES, VkPhysicalDeviceImageRobustnessFeatures);
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES, VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures);
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, VkPhysicalDeviceTransformFeedbackFeaturesEXT);
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT, VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT);
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, VkPhysicalDeviceRobustness2FeaturesEXT);
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT, VkPhysicalDeviceDepthClipEnableFeaturesEXT);
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, VkPhysicalDeviceCustomBorderColorFeaturesEXT);
        // These may not exist in older Vulkan SDKs — guard with #ifdef
        #ifdef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT, VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT);
        #endif
        #ifdef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT, VkPhysicalDeviceExtendedDynamicState3FeaturesEXT);
        #endif
        #ifdef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT, VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT);
        #endif
        #ifdef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT, VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT);
        #endif
        #ifdef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_BIAS_CONTROL_FEATURES_EXT
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_BIAS_CONTROL_FEATURES_EXT, VkPhysicalDeviceDepthBiasControlFeaturesEXT);
        #endif
        #ifdef VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT, VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT);
        #endif
        #ifdef VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT, VkPhysicalDeviceLineRasterizationFeaturesEXT);
        #endif
        #ifdef VK_EXT_MESH_SHADER_EXTENSION_NAME
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT, VkPhysicalDeviceMeshShaderFeaturesEXT);
        #endif
        #ifdef VK_EXT_MULTI_DRAW_EXTENSION_NAME
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_FEATURES_EXT, VkPhysicalDeviceMultiDrawFeaturesEXT);
        #endif
        #ifdef VK_EXT_NON_SEAMLESS_CUBE_MAP_EXTENSION_NAME
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NON_SEAMLESS_CUBE_MAP_FEATURES_EXT, VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT);
        #endif
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT, VkPhysicalDeviceExtendedDynamicStateFeaturesEXT);
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT, VkPhysicalDeviceExtendedDynamicState2FeaturesEXT);
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES, VkPhysicalDeviceScalarBlockLayoutFeatures);
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES, VkPhysicalDeviceUniformBufferStandardLayoutFeatures);
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES, VkPhysicalDeviceVulkanMemoryModelFeatures);
        FB(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_FILTER_MINMAX_PROPERTIES, VkPhysicalDeviceSamplerFilterMinmaxProperties);
        default:
            // Unknown extension feature struct — most have 1 bool field.
            // Safe: 1 VkBool32 (4 bytes) can't reach pNext at offset 8.
            numBools = 1;
            break;
        }
        #undef FB
        for (size_t i = 0; i < numBools; i++) bools[i] = VK_TRUE;
        // BDA (bufferDeviceAddress) is now forwarded: ICD queries host for real GPU addresses.
        // CaptureReplay and MultiDevice are not supported — disable them.
        if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES) {
            bools[1] = VK_FALSE; // bufferDeviceAddressCaptureReplay
            bools[2] = VK_FALSE; // bufferDeviceAddressMultiDevice
        }
        if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            if (numBools >= 47) {
                bools[39] = VK_FALSE; // bufferDeviceAddressCaptureReplay
                bools[40] = VK_FALSE; // bufferDeviceAddressMultiDevice
            }
        }
        // Note: hostImageCopy feature kept TRUE (DXVK requires it for Vulkan 1.4).
        // The actual vkCopyMemoryToImage functions return nullptr via nullPrefixes,
        // forcing DXVK to fallback to CmdCopyBufferToImage at runtime.

        // --- Disable dangerous features in pNext structs (matching physDeviceFeatures) ---
        // These must be consistent with the base VkPhysicalDeviceFeatures we report.
        if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES) {
            auto* f = reinterpret_cast<VkPhysicalDeviceVulkan11Features*>(next);
            // shaderDrawParameters: keep TRUE (DXVK needs it)
            fprintf(stderr, "[ICD]   vk11.shaderDrawParameters=%d\n", (int)f->shaderDrawParameters);
            fflush(stderr);
        }
        if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            auto* f = reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(next);
            // Vk12 has shaderFloat16/shaderInt8 etc — keep those TRUE.
            // But disable shaderOutputLayer if geometry shader is off
            // (shaderOutputLayer requires geometry shader or mesh shader).
            // Actually Vk12 doesn't duplicate geometry/tessellation flags, so nothing extra here.
            (void)f;
        }

        // VkPhysicalDeviceMeshShaderFeaturesEXT: disable if we report no GS
        // (mesh shader is an alternative pipeline, not currently supported)
        #ifdef VK_EXT_MESH_SHADER_EXTENSION_NAME
        if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT) {
            auto* f = reinterpret_cast<VkPhysicalDeviceMeshShaderFeaturesEXT*>(next);
            f->taskShader = VK_FALSE;
            f->meshShader = VK_FALSE;
            f->multiviewMeshShader = VK_FALSE;
            f->primitiveFragmentShadingRateMeshShader = VK_FALSE;
            f->meshShaderQueries = VK_FALSE;
        }
        #endif

        next = next->pNext;
    }
    fprintf(stderr, "[ICD] vkGetPhysicalDeviceFeatures2 DONE\n"); fflush(stderr);
}

static void VKAPI_CALL icd_vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties* p) {
    fprintf(stderr, "[ICD] vkGetPhysicalDeviceMemoryProperties\n"); fflush(stderr);
    *p = g_icd.memProps;
}

static void VKAPI_CALL icd_vkGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties2* p) {
    fprintf(stderr, "[ICD] vkGetPhysicalDeviceMemoryProperties2\n"); fflush(stderr);
    icd_vkGetPhysicalDeviceMemoryProperties(pd, &p->memoryProperties);
    // Handle pNext: VkPhysicalDeviceMemoryBudgetPropertiesEXT
    VkBaseOutStructure* next = reinterpret_cast<VkBaseOutStructure*>(p->pNext);
    while (next) {
        fprintf(stderr, "[ICD]   memprops2 pNext sType=%u\n", next->sType); fflush(stderr);
        if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT) {
            auto* budget = reinterpret_cast<VkPhysicalDeviceMemoryBudgetPropertiesEXT*>(next);
            budget->heapBudget[0] = 4ull * 1024 * 1024 * 1024;
            budget->heapUsage[0] = 0;
            budget->heapBudget[1] = 8ull * 1024 * 1024 * 1024;
            budget->heapUsage[1] = 0;
        }
        next = next->pNext;
    }
    fprintf(stderr, "[ICD] vkGetPhysicalDeviceMemoryProperties2 DONE\n"); fflush(stderr);
}

static void VKAPI_CALL icd_vkGetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice, uint32_t* pCount, VkQueueFamilyProperties* p)
{
    if (!p) { *pCount = (uint32_t)g_icd.queueFamilies.size(); return; }
    uint32_t n = (uint32_t)g_icd.queueFamilies.size();
    if (*pCount < n) n = *pCount;
    memcpy(p, g_icd.queueFamilies.data(), n * sizeof(VkQueueFamilyProperties));
    *pCount = n;
}

static void VKAPI_CALL icd_vkGetPhysicalDeviceQueueFamilyProperties2(
    VkPhysicalDevice, uint32_t* pCount, VkQueueFamilyProperties2* p)
{
    if (!p) { *pCount = (uint32_t)g_icd.queueFamilies.size(); return; }
    uint32_t n = (uint32_t)g_icd.queueFamilies.size();
    if (*pCount < n) n = *pCount;
    for (uint32_t i = 0; i < n; i++)
        p[i].queueFamilyProperties = g_icd.queueFamilies[i];
    *pCount = n;
}

static VkResult VKAPI_CALL icd_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice, const char*, uint32_t* pCount, VkExtensionProperties* p)
{
    if (!p) { *pCount = (uint32_t)g_icd.deviceExtensions.size(); return VK_SUCCESS; }
    uint32_t n = (uint32_t)g_icd.deviceExtensions.size();
    if (*pCount < n) { memcpy(p, g_icd.deviceExtensions.data(), *pCount * sizeof(VkExtensionProperties)); return VK_INCOMPLETE; }
    memcpy(p, g_icd.deviceExtensions.data(), n * sizeof(VkExtensionProperties));
    *pCount = n;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL icd_vkEnumerateInstanceExtensionProperties(
    const char*, uint32_t* pCount, VkExtensionProperties* p)
{
    // Report surface extensions
    static VkExtensionProperties exts[] = {
        { VK_KHR_SURFACE_EXTENSION_NAME, 1 },
        { VK_KHR_WIN32_SURFACE_EXTENSION_NAME, 1 },
    };
    if (!p) { *pCount = 2; return VK_SUCCESS; }
    uint32_t n = (*pCount < 2) ? *pCount : 2;
    memcpy(p, exts, n * sizeof(VkExtensionProperties));
    *pCount = n;
    return (n < 2) ? VK_INCOMPLETE : VK_SUCCESS;
}

static void VKAPI_CALL icd_vkGetPhysicalDeviceFormatProperties(
    VkPhysicalDevice, VkFormat format, VkFormatProperties* p)
{
    // Report broad format support — depth/stencil formats get appropriate bits
    bool isDS = (format >= VK_FORMAT_D16_UNORM && format <= VK_FORMAT_D32_SFLOAT_S8_UINT);
    VkFormatFeatureFlags optBits =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
        VK_FORMAT_FEATURE_BLIT_SRC_BIT |
        (isDS ? VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
              : (VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                 VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                 VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                 VK_FORMAT_FEATURE_BLIT_DST_BIT));
    p->linearTilingFeatures = isDS ? 0 : optBits;
    p->optimalTilingFeatures = optBits;
    p->bufferFeatures = isDS ? 0 :
        (VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT | VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT);
}

static void VKAPI_CALL icd_vkGetPhysicalDeviceFormatProperties2(
    VkPhysicalDevice pd, VkFormat format, VkFormatProperties2* p)
{
    icd_vkGetPhysicalDeviceFormatProperties(pd, format, &p->formatProperties);
    // Fill VkFormatProperties3 in pNext (Vulkan 1.3) — DXVK uses this for format feature checks
    auto* base = reinterpret_cast<VkBaseOutStructure*>(p->pNext);
    while (base) {
        if (base->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3) {
            // VkFormatProperties3: sType, pNext, linearTilingFeatures, optimalTilingFeatures, bufferFeatures (all VkFormatFeatureFlags2 = uint64)
            auto* fp3 = reinterpret_cast<VkFormatProperties3*>(base);
            fp3->linearTilingFeatures = (VkFormatFeatureFlags2)p->formatProperties.linearTilingFeatures;
            fp3->optimalTilingFeatures = (VkFormatFeatureFlags2)p->formatProperties.optimalTilingFeatures;
            fp3->bufferFeatures = (VkFormatFeatureFlags2)p->formatProperties.bufferFeatures;
            break;
        }
        base = base->pNext;
    }
}

// --- Surface ---
static VkResult VKAPI_CALL icd_vkCreateWin32SurfaceKHR(
    VkInstance, const VkWin32SurfaceCreateInfoKHR* pInfo, const VkAllocationCallbacks*, VkSurfaceKHR* p)
{
    *p = (VkSurfaceKHR)g_icd.handles.alloc();
    if (pInfo && pInfo->hwnd) {
        g_icd.presentHwnd = pInfo->hwnd;
    }
    return VK_SUCCESS;
}

static void VKAPI_CALL icd_vkDestroySurfaceKHR(VkInstance, VkSurfaceKHR, const VkAllocationCallbacks*) {}

static VkResult VKAPI_CALL icd_vkGetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32* p)
{
    *p = VK_TRUE;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL icd_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR* p)
{
    memset(p, 0, sizeof(*p));
    p->minImageCount = 2;
    p->maxImageCount = 8;
    p->currentExtent = g_icd.swapchainExtent;
    p->minImageExtent = { 1, 1 };
    p->maxImageExtent = { 16384, 16384 };
    p->maxImageArrayLayers = 1;
    p->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    p->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    p->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    p->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL icd_vkGetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice, VkSurfaceKHR, uint32_t* pCount, VkSurfaceFormatKHR* p)
{
    if (!p) { *pCount = (uint32_t)g_icd.surfaceFormats.size(); return VK_SUCCESS; }
    uint32_t n = (uint32_t)g_icd.surfaceFormats.size();
    if (*pCount < n) n = *pCount;
    memcpy(p, g_icd.surfaceFormats.data(), n * sizeof(VkSurfaceFormatKHR));
    *pCount = n;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL icd_vkGetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice, VkSurfaceKHR, uint32_t* pCount, VkPresentModeKHR* p)
{
    if (!p) { *pCount = 1; return VK_SUCCESS; }
    p[0] = VK_PRESENT_MODE_FIFO_KHR;
    *pCount = 1;
    return VK_SUCCESS;
}

// --- Device ---
static VkResult VKAPI_CALL icd_vkCreateDevice(
    VkPhysicalDevice, const VkDeviceCreateInfo* pInfo, const VkAllocationCallbacks*, VkDevice* pDevice)
{
    // Connect to host now (deferred from vkCreateInstance)
    if (!g_icd.connected) {
        const char* hostAddr = getenv("VBOX_GPU_HOST");
        if (!hostAddr) hostAddr = "127.0.0.1";
        const char* portStr = getenv("VBOX_GPU_PORT");
        uint16_t port = portStr ? (uint16_t)atoi(portStr) : DEFAULT_PORT;

        if (!g_icd.connectToHost(hostAddr, port)) {
            fprintf(stderr, "[ICD] Failed to connect to Host at %s:%u\n", hostAddr, port);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        fprintf(stderr, "[ICD] Connected to Host at %s:%u\n", hostAddr, port);
    }

    uint64_t id = g_icd.handles.alloc();
    *pDevice = reinterpret_cast<VkDevice>(makeDispatchable(id));
    fprintf(stderr, "[ICD] vkCreateDevice: id=%llu handle=%p extensions=%u\n",
            (unsigned long long)id, (void*)*pDevice, pInfo ? pInfo->enabledExtensionCount : 0);
    fflush(stderr);
    return VK_SUCCESS;
}

static void VKAPI_CALL icd_vkDestroyDevice(VkDevice device, const VkAllocationCallbacks*) {
    // Don't stop recv thread or disconnect — DXUT may destroy and recreate devices.
    // The TCP connection is per-ICD-lifetime, not per-device.
    if (device) delete reinterpret_cast<DispatchableHandle*>(device);
}

static void VKAPI_CALL icd_vkGetDeviceQueue(VkDevice, uint32_t family, uint32_t idx, VkQueue* pQueue) {
    static DispatchableHandle queueHandle = { nullptr, 2 };
    *pQueue = reinterpret_cast<VkQueue>(&queueHandle);
    fprintf(stderr, "[ICD] vkGetDeviceQueue: family=%u idx=%u → %p\n", family, idx, (void*)*pQueue);
    fflush(stderr);
}

static void VKAPI_CALL icd_vkGetDeviceQueue2(VkDevice, const VkDeviceQueueInfo2* pInfo, VkQueue* pQueue) {
    static DispatchableHandle queueHandle2 = { nullptr, 2 };
    *pQueue = reinterpret_cast<VkQueue>(&queueHandle2);
    fprintf(stderr, "[ICD] vkGetDeviceQueue2: family=%u idx=%u → %p\n",
            pInfo ? pInfo->queueFamilyIndex : 0, pInfo ? pInfo->queueIndex : 0, (void*)*pQueue);
    fflush(stderr);
}

// Vulkan 1.2+ device functions DXVK needs
static VkResult VKAPI_CALL icd_vkBindBufferMemory2(VkDevice, uint32_t bindInfoCount, const VkBindBufferMemoryInfo* pBindInfos) {
    if (!pBindInfos) return VK_SUCCESS;
    for (uint32_t i = 0; i < bindInfoCount; i++) {
        g_icd.bufferBindings[(uint64_t)pBindInfos[i].buffer] = {
            (uint64_t)pBindInfos[i].memory,
            pBindInfos[i].memoryOffset
        };
        g_icd.encoder.cmdBindBufferMemory(1, (uint64_t)pBindInfos[i].buffer,
            (uint64_t)pBindInfos[i].memory, pBindInfos[i].memoryOffset);
    }
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL icd_vkBindImageMemory2(VkDevice, uint32_t bindInfoCount, const VkBindImageMemoryInfo* pBindInfos) {
    if (!pBindInfos) return VK_SUCCESS;
    for (uint32_t i = 0; i < bindInfoCount; i++) {
        g_icd.encoder.cmdBindImageMemory(1, (uint64_t)pBindInfos[i].image,
            (uint64_t)pBindInfos[i].memory, pBindInfos[i].memoryOffset);
    }
    return VK_SUCCESS;
}
static VkDeviceAddress VKAPI_CALL icd_vkGetBufferDeviceAddress(VkDevice, const VkBufferDeviceAddressInfo* pInfo) {
    if (!pInfo || !pInfo->buffer) return 0;
    uint64_t bufferId = (uint64_t)pInfo->buffer;
    VkDeviceAddress addr = g_icd.syncGetBufferDeviceAddress(bufferId);
    // Emit RecordBDA once per buffer so replay can patch BDA values in WriteMemory payloads.
    if (addr != 0) {
        bool shouldEmit = false;
        {
            std::lock_guard<std::mutex> lk(g_icd.bdaMutex_);
            if (!g_icd.bdaRecorded_.count(bufferId)) {
                g_icd.bdaRecorded_.insert(bufferId);
                shouldEmit = true;
            }
        }
        if (shouldEmit)
            g_icd.encoder.cmdBridgeRecordBDA(bufferId, (uint64_t)addr);
    }
    return addr;
}
static uint64_t VKAPI_CALL icd_vkGetBufferOpaqueCaptureAddress(VkDevice, const VkBufferDeviceAddressInfo*) { return 0; }
static uint64_t VKAPI_CALL icd_vkGetDeviceMemoryOpaqueCaptureAddress(VkDevice, const VkDeviceMemoryOpaqueCaptureAddressInfo*) { return 0; }
static VkResult VKAPI_CALL icd_vkGetSemaphoreCounterValue(VkDevice, VkSemaphore, uint64_t* p) { *p = 0; return VK_SUCCESS; }
static VkResult VKAPI_CALL icd_vkWaitSemaphores(VkDevice, const VkSemaphoreWaitInfo*, uint64_t) { return VK_SUCCESS; }
static VkResult VKAPI_CALL icd_vkSignalSemaphore(VkDevice, const VkSemaphoreSignalInfo*) { return VK_SUCCESS; }

// Vulkan 1.3 dynamic rendering
static void VKAPI_CALL icd_vkCmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo* pInfo) {
    uint32_t loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    uint32_t storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    float cr = 0, cg = 0, cb_ = 0, ca = 1;
    if (pInfo && pInfo->colorAttachmentCount > 0 && pInfo->pColorAttachments) {
        loadOp = pInfo->pColorAttachments[0].loadOp;
        storeOp = pInfo->pColorAttachments[0].storeOp;
        cr = pInfo->pColorAttachments[0].clearValue.color.float32[0];
        cg = pInfo->pColorAttachments[0].clearValue.color.float32[1];
        cb_ = pInfo->pColorAttachments[0].clearValue.color.float32[2];
        ca = pInfo->pColorAttachments[0].clearValue.color.float32[3];
    }
    // Extract the imageView from the first color attachment
    uint64_t imageViewId = 0;
    if (pInfo && pInfo->colorAttachmentCount > 0 && pInfo->pColorAttachments)
        imageViewId = (uint64_t)pInfo->pColorAttachments[0].imageView;

    // Check if this imageView references a swapchain image (sentinel 0xFFF00000+i)
    // If so, send imageViewId=0 to tell host to use swapchain target
    auto ivIt = g_icd.imageViewToImage.find(imageViewId);
    if (ivIt != g_icd.imageViewToImage.end() && (ivIt->second & 0xFFF00000) == 0xFFF00000)
        imageViewId = 0;

    // Depth attachment
    uint32_t hasDepth = 0;
    uint64_t depthViewId = 0;
    uint32_t depthLoadOp = 0, depthStoreOp = 0;
    float clearDepth = 1.0f;
    if (pInfo && pInfo->pDepthAttachment && pInfo->pDepthAttachment->imageView != VK_NULL_HANDLE) {
        hasDepth = 1;
        depthViewId = (uint64_t)pInfo->pDepthAttachment->imageView;
        depthLoadOp = pInfo->pDepthAttachment->loadOp;
        depthStoreOp = pInfo->pDepthAttachment->storeOp;
        clearDepth = pInfo->pDepthAttachment->clearValue.depthStencil.depth;
    }

    g_icd.encoder.cmdBeginRendering(toId(cb),
        pInfo ? pInfo->renderArea.offset.x : 0,
        pInfo ? pInfo->renderArea.offset.y : 0,
        pInfo ? pInfo->renderArea.extent.width : 800,
        pInfo ? pInfo->renderArea.extent.height : 600,
        loadOp, storeOp, cr, cg, cb_, ca, imageViewId,
        hasDepth, depthViewId, depthLoadOp, depthStoreOp, clearDepth);
}
static void VKAPI_CALL icd_vkCmdEndRendering(VkCommandBuffer cb) {
    g_icd.encoder.cmdEndRendering(toId(cb));
}
static void VKAPI_CALL icd_vkCmdSetCullMode(VkCommandBuffer cb, VkCullModeFlags mode) {
    g_icd.encoder.cmdSetCullMode(toId(cb), mode);
}
static void VKAPI_CALL icd_vkCmdSetFrontFace(VkCommandBuffer cb, VkFrontFace face) {
    g_icd.encoder.cmdSetFrontFace(toId(cb), face);
}
static void VKAPI_CALL icd_vkCmdSetPrimitiveTopology(VkCommandBuffer cb, VkPrimitiveTopology topology) {
    g_icd.encoder.cmdSetPrimitiveTopology(toId(cb), (uint32_t)topology);
}
static void VKAPI_CALL icd_vkCmdSetViewportWithCount(VkCommandBuffer cb, uint32_t count, const VkViewport* vps) {
    if (count > 0 && vps)
        g_icd.encoder.cmdSetViewport(toId(cb), vps[0].x, vps[0].y, vps[0].width, vps[0].height, vps[0].minDepth, vps[0].maxDepth);
}
static void VKAPI_CALL icd_vkCmdSetScissorWithCount(VkCommandBuffer cb, uint32_t count, const VkRect2D* rects) {
    if (count > 0 && rects)
        g_icd.encoder.cmdSetScissor(toId(cb), rects[0].offset.x, rects[0].offset.y, rects[0].extent.width, rects[0].extent.height);
}
static void VKAPI_CALL icd_vkCmdSetDepthTestEnable(VkCommandBuffer cb, VkBool32 enable) {
    g_icd.encoder.cmdSetDepthTestEnable(toId(cb), enable);
}
static void VKAPI_CALL icd_vkCmdSetDepthWriteEnable(VkCommandBuffer cb, VkBool32 enable) {
    g_icd.encoder.cmdSetDepthWriteEnable(toId(cb), enable);
}
static void VKAPI_CALL icd_vkCmdSetDepthCompareOp(VkCommandBuffer cb, VkCompareOp op) {
    g_icd.encoder.cmdSetDepthCompareOp(toId(cb), (uint32_t)op);
}
static void VKAPI_CALL icd_vkCmdSetDepthBoundsTestEnable(VkCommandBuffer cb, VkBool32 enable) {
    g_icd.encoder.cmdSetDepthBoundsTestEnable(toId(cb), enable);
}
static void VKAPI_CALL icd_vkCmdSetStencilTestEnable(VkCommandBuffer, VkBool32) {}
static void VKAPI_CALL icd_vkCmdSetStencilOp(VkCommandBuffer, VkStencilFaceFlags, VkStencilOp, VkStencilOp, VkStencilOp, VkCompareOp) {}
static void VKAPI_CALL icd_vkCmdSetRasterizerDiscardEnable(VkCommandBuffer, VkBool32) {}
static void VKAPI_CALL icd_vkCmdSetDepthBiasEnable(VkCommandBuffer cb, VkBool32 enable) {
    g_icd.encoder.cmdSetDepthBiasEnable(toId(cb), enable);
}
static void VKAPI_CALL icd_vkCmdSetPrimitiveRestartEnable(VkCommandBuffer, VkBool32) {}
static void VKAPI_CALL icd_vkCmdBindVertexBuffers2(VkCommandBuffer cb, uint32_t firstBinding, uint32_t bindingCount,
    const VkBuffer* pBuffers, const VkDeviceSize* pOffsets, const VkDeviceSize* pSizes, const VkDeviceSize* pStrides) {
    std::vector<uint64_t> ids(bindingCount), offs(bindingCount), szs(bindingCount), strs(bindingCount);
    for (uint32_t i = 0; i < bindingCount; i++) {
        ids[i] = (uint64_t)pBuffers[i];
        offs[i] = pOffsets[i];
        szs[i] = pSizes ? pSizes[i] : VK_WHOLE_SIZE;
        strs[i] = pStrides ? pStrides[i] : 0;
    }
    g_icd.encoder.cmdBindVertexBuffers(toId(cb), firstBinding, bindingCount,
        ids.data(), offs.data(), szs.data(), strs.data());
}
static void VKAPI_CALL icd_vkCmdSetDepthBounds(VkCommandBuffer, float, float) {}
static void VKAPI_CALL icd_vkCmdDrawIndirect(VkCommandBuffer, VkBuffer, VkDeviceSize, uint32_t, uint32_t) {}
static void VKAPI_CALL icd_vkCmdDrawIndexedIndirect(VkCommandBuffer, VkBuffer, VkDeviceSize, uint32_t, uint32_t) {}

// Descriptor update template
static VkResult VKAPI_CALL icd_vkCreateDescriptorUpdateTemplate(VkDevice,
    const VkDescriptorUpdateTemplateCreateInfo* pInfo, const VkAllocationCallbacks*, VkDescriptorUpdateTemplate* p)
{
    uint64_t id = g_icd.handles.alloc();
    *p = (VkDescriptorUpdateTemplate)id;
    // Save template entries so UpdateDescriptorSetWithTemplate can interpret pData
    IcdState::DescriptorTemplateInfo info;
    info.entries.assign(pInfo->pDescriptorUpdateEntries,
                        pInfo->pDescriptorUpdateEntries + pInfo->descriptorUpdateEntryCount);
    g_icd.descriptorTemplates[id] = std::move(info);
    fprintf(stderr, "[ICD] CreateDescriptorUpdateTemplate: id=%llu entries=%u\n",
            (unsigned long long)id, pInfo->descriptorUpdateEntryCount);
    return VK_SUCCESS;
}
static void VKAPI_CALL icd_vkDestroyDescriptorUpdateTemplate(VkDevice, VkDescriptorUpdateTemplate t, const VkAllocationCallbacks*) {
    g_icd.descriptorTemplates.erase((uint64_t)t);
}
static void VKAPI_CALL icd_vkUpdateDescriptorSetWithTemplate(VkDevice,
    VkDescriptorSet dstSet, VkDescriptorUpdateTemplate tmpl, const void* pData)
{
    uint64_t tmplId = (uint64_t)tmpl;
    auto it = g_icd.descriptorTemplates.find(tmplId);
    if (it == g_icd.descriptorTemplates.end()) return;

    // Convert template update to regular VkWriteDescriptorSet array
    const auto& entries = it->second.entries;
    std::vector<VkWriteDescriptorSet> writes(entries.size());
    std::vector<std::vector<VkDescriptorImageInfo>> allImageInfos(entries.size());
    std::vector<std::vector<VkDescriptorBufferInfo>> allBufferInfos(entries.size());

    for (size_t i = 0; i < entries.size(); i++) {
        const auto& e = entries[i];
        writes[i] = {};
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = dstSet;
        writes[i].dstBinding = e.dstBinding;
        writes[i].dstArrayElement = e.dstArrayElement;
        writes[i].descriptorCount = e.descriptorCount;
        writes[i].descriptorType = e.descriptorType;

        bool isImage = (e.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
                        e.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                        e.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                        e.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
                        e.descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT);
        bool isBuffer = (e.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
                         e.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
                         e.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
                         e.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC);

        if (isImage) {
            allImageInfos[i].resize(e.descriptorCount);
            for (uint32_t j = 0; j < e.descriptorCount; j++) {
                const auto* src = reinterpret_cast<const VkDescriptorImageInfo*>(
                    static_cast<const char*>(pData) + e.offset + j * e.stride);
                allImageInfos[i][j] = *src;
            }
            writes[i].pImageInfo = allImageInfos[i].data();
        } else if (isBuffer) {
            allBufferInfos[i].resize(e.descriptorCount);
            for (uint32_t j = 0; j < e.descriptorCount; j++) {
                const auto* src = reinterpret_cast<const VkDescriptorBufferInfo*>(
                    static_cast<const char*>(pData) + e.offset + j * e.stride);
                allBufferInfos[i][j] = *src;
            }
            writes[i].pBufferInfo = allBufferInfos[i].data();
        }
    }

    // Log each descriptor binding for debugging
    for (size_t i = 0; i < writes.size(); i++) {
        fprintf(stderr, "[ICD] DescWrite[%zu]: binding=%u type=%u count=%u",
                i, writes[i].dstBinding, writes[i].descriptorType, writes[i].descriptorCount);
        if (writes[i].pImageInfo && writes[i].descriptorCount > 0) {
            fprintf(stderr, " img[0]=(sam=%llu view=%llu layout=%u)",
                    (unsigned long long)(uint64_t)writes[i].pImageInfo[0].sampler,
                    (unsigned long long)(uint64_t)writes[i].pImageInfo[0].imageView,
                    writes[i].pImageInfo[0].imageLayout);
        }
        fprintf(stderr, "\n");
    }
    // Encode as regular UpdateDescriptorSets
    g_icd.encoder.cmdUpdateDescriptorSets(1, (uint32_t)writes.size(), writes.data());
}

// vkCmdBindDescriptorSets2 (Vulkan 1.4 / VK_KHR_maintenance6)
// Wraps to the standard vkCmdBindDescriptorSets
static void VKAPI_CALL icd_vkCmdBindDescriptorSets2KHR(
    VkCommandBuffer cb, const void* pBindInfo /* VkBindDescriptorSetsInfoKHR* */)
{
    // VkBindDescriptorSetsInfoKHR layout:
    // sType, pNext, stageFlags, layout, firstSet, descriptorSetCount, pDescriptorSets,
    // dynamicOffsetCount, pDynamicOffsets
    struct BindInfo {
        VkStructureType sType;
        const void* pNext;
        VkShaderStageFlags stageFlags;
        VkPipelineLayout layout;
        uint32_t firstSet;
        uint32_t descriptorSetCount;
        const VkDescriptorSet* pDescriptorSets;
        uint32_t dynamicOffsetCount;
        const uint32_t* pDynamicOffsets;
    };
    const auto* info = static_cast<const BindInfo*>(pBindInfo);

    std::vector<uint64_t> setIds(info->descriptorSetCount);
    for (uint32_t i = 0; i < info->descriptorSetCount; i++)
        setIds[i] = (uint64_t)info->pDescriptorSets[i];

    g_icd.encoder.cmdBindDescriptorSets(toId(cb),
        VK_PIPELINE_BIND_POINT_GRAPHICS, (uint64_t)info->layout,
        info->firstSet, info->descriptorSetCount, setIds.data(),
        info->dynamicOffsetCount, info->pDynamicOffsets);
}

// Push descriptor set (VK_KHR_push_descriptor)
static void VKAPI_CALL icd_vkCmdPushDescriptorSetKHR(
    VkCommandBuffer cb, VkPipelineBindPoint bindPoint,
    VkPipelineLayout layout, uint32_t set,
    uint32_t writeCount, const VkWriteDescriptorSet* pWrites)
{
    // Flush buffer data for any buffer descriptors
    for (uint32_t i = 0; i < writeCount; i++) {
        if (pWrites[i].pBufferInfo) {
            for (uint32_t j = 0; j < pWrites[i].descriptorCount; j++) {
                auto& bi = pWrites[i].pBufferInfo[j];
                if (bi.buffer)
                    g_icd.flushBufferRange((uint64_t)bi.buffer, bi.offset, bi.range);
            }
        }
    }

    std::vector<uint64_t> dstBindings;
    std::vector<uint32_t> descCounts, descTypes;
    std::vector<uint64_t> samplerIds, imageViewIds;
    std::vector<uint32_t> imageLayouts;
    std::vector<uint64_t> bufferIds, bufferOffsets, bufferRanges;

    for (uint32_t i = 0; i < writeCount; i++) {
        dstBindings.push_back(pWrites[i].dstBinding);
        descCounts.push_back(pWrites[i].descriptorCount);
        descTypes.push_back(pWrites[i].descriptorType);
        for (uint32_t j = 0; j < pWrites[i].descriptorCount; j++) {
            if (pWrites[i].pImageInfo) {
                samplerIds.push_back((uint64_t)pWrites[i].pImageInfo[j].sampler);
                imageViewIds.push_back((uint64_t)pWrites[i].pImageInfo[j].imageView);
                imageLayouts.push_back(pWrites[i].pImageInfo[j].imageLayout);
            } else {
                samplerIds.push_back(0); imageViewIds.push_back(0); imageLayouts.push_back(0);
            }
            if (pWrites[i].pBufferInfo) {
                bufferIds.push_back((uint64_t)pWrites[i].pBufferInfo[j].buffer);
                bufferOffsets.push_back(pWrites[i].pBufferInfo[j].offset);
                bufferRanges.push_back(pWrites[i].pBufferInfo[j].range);
            } else {
                bufferIds.push_back(0); bufferOffsets.push_back(0); bufferRanges.push_back(0);
            }
        }
    }

    g_icd.encoder.cmdPushDescriptorSet(
        toId(cb), bindPoint, (uint64_t)layout, set,
        writeCount, dstBindings.data(), descCounts.data(), descTypes.data(),
        samplerIds.data(), imageViewIds.data(), imageLayouts.data(),
        bufferIds.data(), bufferOffsets.data(), bufferRanges.data(),
        (uint32_t)samplerIds.size());
}

// Push descriptor set with template (VK_KHR_push_descriptor + descriptor_update_template)
static void VKAPI_CALL icd_vkCmdPushDescriptorSetWithTemplateKHR(
    VkCommandBuffer cb, VkDescriptorUpdateTemplate tmpl,
    VkPipelineLayout layout, uint32_t set, const void* pData)
{
    uint64_t tmplId = (uint64_t)tmpl;
    auto it = g_icd.descriptorTemplates.find(tmplId);
    if (it == g_icd.descriptorTemplates.end()) return;

    const auto& entries = it->second.entries;
    std::vector<uint64_t> dstBindings;
    std::vector<uint32_t> descCounts, descTypes;
    std::vector<uint64_t> samplerIds, imageViewIds;
    std::vector<uint32_t> imageLayouts;
    std::vector<uint64_t> bufferIds, bufferOffsets, bufferRanges;

    for (const auto& e : entries) {
        dstBindings.push_back(e.dstBinding);
        descCounts.push_back(e.descriptorCount);
        descTypes.push_back(e.descriptorType);

        bool isImage = (e.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
                        e.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                        e.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                        e.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
                        e.descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT);

        for (uint32_t j = 0; j < e.descriptorCount; j++) {
            if (isImage) {
                const auto* info = reinterpret_cast<const VkDescriptorImageInfo*>(
                    static_cast<const char*>(pData) + e.offset + j * e.stride);
                samplerIds.push_back((uint64_t)info->sampler);
                imageViewIds.push_back((uint64_t)info->imageView);
                imageLayouts.push_back(info->imageLayout);
                bufferIds.push_back(0); bufferOffsets.push_back(0); bufferRanges.push_back(0);
            } else {
                samplerIds.push_back(0); imageViewIds.push_back(0); imageLayouts.push_back(0);
                const auto* info = reinterpret_cast<const VkDescriptorBufferInfo*>(
                    static_cast<const char*>(pData) + e.offset + j * e.stride);
                bufferIds.push_back((uint64_t)info->buffer);
                bufferOffsets.push_back(info->offset);
                bufferRanges.push_back(info->range);
            }
        }
    }

    g_icd.encoder.cmdPushDescriptorSet(
        toId(cb), VK_PIPELINE_BIND_POINT_GRAPHICS, (uint64_t)layout, set,
        (uint32_t)entries.size(), dstBindings.data(), descCounts.data(), descTypes.data(),
        samplerIds.data(), imageViewIds.data(), imageLayouts.data(),
        bufferIds.data(), bufferOffsets.data(), bufferRanges.data(),
        (uint32_t)samplerIds.size());
}

// Private data
static VkResult VKAPI_CALL icd_vkCreatePrivateDataSlot(VkDevice, const VkPrivateDataSlotCreateInfo*, const VkAllocationCallbacks*, VkPrivateDataSlot* p) {
    *p = (VkPrivateDataSlot)g_icd.handles.alloc(); return VK_SUCCESS;
}
static void VKAPI_CALL icd_vkDestroyPrivateDataSlot(VkDevice, VkPrivateDataSlot, const VkAllocationCallbacks*) {}
static VkResult VKAPI_CALL icd_vkSetPrivateData(VkDevice, VkObjectType, uint64_t, VkPrivateDataSlot, uint64_t) { return VK_SUCCESS; }
static void VKAPI_CALL icd_vkGetPrivateData(VkDevice, VkObjectType, uint64_t, VkPrivateDataSlot, uint64_t* p) { *p = 0; }

static VkResult VKAPI_CALL icd_vkDeviceWaitIdle(VkDevice) {
    return VK_SUCCESS;
}

// --- Swapchain ---
static VkResult VKAPI_CALL icd_vkCreateSwapchainKHR(
    VkDevice, const VkSwapchainCreateInfoKHR* pInfo, const VkAllocationCallbacks*, VkSwapchainKHR* p)
{
    uint64_t id = g_icd.handles.alloc();
    *p = (VkSwapchainKHR)id;

    g_icd.swapchainExtent = pInfo->imageExtent;
    g_icd.swapchainFormat = pInfo->imageFormat;

    // Tell host to create swapchain
    g_icd.encoder.cmdBridgeCreateSwapchain(1, id,
        pInfo->imageExtent.width, pInfo->imageExtent.height,
        g_icd.swapchainImageCount);

    return VK_SUCCESS;
}

static void VKAPI_CALL icd_vkDestroySwapchainKHR(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*) {}

static VkResult VKAPI_CALL icd_vkGetSwapchainImagesKHR(
    VkDevice, VkSwapchainKHR, uint32_t* pCount, VkImage* pImages)
{
    if (!pImages) { *pCount = g_icd.swapchainImageCount; return VK_SUCCESS; }
    uint32_t n = g_icd.swapchainImageCount;
    if (*pCount < n) n = *pCount;
    for (uint32_t i = 0; i < n; i++)
        pImages[i] = (VkImage)(0xFFF00000ull + i); // sentinel values
    *pCount = n;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL icd_vkAcquireNextImageKHR(
    VkDevice, VkSwapchainKHR, uint64_t timeout, VkSemaphore, VkFence, uint32_t* pIndex)
{
    // First acquire (before any present): return image 0 immediately — no response yet.
    if (!g_icd.firstPresented_) {
        *pIndex = 0;
        return VK_SUCCESS;
    }
    // Wait for recv thread to deliver imageIndex from the last QueuePresent response.
    std::unique_lock<std::mutex> lock(g_icd.acquireMutex_);
    auto waitMs = (timeout == UINT64_MAX)
        ? std::chrono::milliseconds(5000)
        : std::chrono::milliseconds(timeout / 1000000 + 1);
    g_icd.acquireCV_.wait_for(lock, waitMs, []{ return g_icd.imageIndexReady_; });
    *pIndex = g_icd.currentImageIndex;
    g_icd.imageIndexReady_ = false;
    // Clear per-frame buffer flush dedup set: new frame starts, buffers may be updated.
    {
        std::lock_guard<std::mutex> lk(g_icd.flushedBuffersMutex_);
        g_icd.flushedBuffersThisFrame_.clear();
    }
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL icd_vkQueuePresentKHR(VkQueue q, const VkPresentInfoKHR* pInfo)
{
    g_icd.encoder.cmdBridgeQueuePresent(2, // H_QUEUE
        ndToId((uint64_t)pInfo->pSwapchains[0]),
        pInfo->waitSemaphoreCount > 0 ? ndToId((uint64_t)pInfo->pWaitSemaphores[0]) : 0);

    // Walk pNext for VkSwapchainPresentFenceInfoEXT (sType=1000275001) —
    // DXVK presenter attaches fences that must be signaled after present completes.
    // Forward them as empty QueueSubmit so the host signals them after present.
    {
        struct PresentFenceInfo { VkStructureType sType; const void* pNext; uint32_t swapchainCount; const VkFence* pFences; };
        const VkBaseInStructure* pNext = reinterpret_cast<const VkBaseInStructure*>(pInfo->pNext);
        while (pNext) {
            if (pNext->sType == (VkStructureType)1000275001) { // VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT
                auto* fi = reinterpret_cast<const PresentFenceInfo*>(pNext);
                for (uint32_t i = 0; i < pInfo->swapchainCount; i++) {
                    if (fi->pFences[i])
                        g_icd.encoder.cmdQueueSubmit(toId(q), 0, 0, 0, (uint64_t)fi->pFences[i]);
                }
                break;
            }
            pNext = pNext->pNext;
        }
    }

    // Fire-and-forget: send batch to host. Recv thread will receive the response,
    // update currentImageIndex, blit the frame, and signal acquireCV_.
    g_icd.sendBatch(true); // true = present batch
    g_icd.firstPresented_ = true;

#ifdef VBOXGPU_DEBUG_SCREENSHOTS
    // Debug: save first few returned frames to BMP for verification
    static int dbgFrameCount = 0;
    dbgFrameCount++;
    if (g_icd.frameValid && (dbgFrameCount == 5 || dbgFrameCount == 30)) {
        char path[256];
        snprintf(path, sizeof(path), "S:\\bld\\vboxgpu\\guest_frame%d.bmp", dbgFrameCount);
        FILE* f = fopen(path, "wb");
        if (f) {
            uint32_t w = g_icd.frameWidth, h = g_icd.frameHeight;
            uint32_t rowStride = (w * 4 + 3u) & ~3u;
            uint32_t pixelDataSize = rowStride * h;
            uint32_t fileSize = 14 + 40 + pixelDataSize;
            uint8_t fh[14] = {}; fh[0]='B'; fh[1]='M';
            *(uint32_t*)(fh+2) = fileSize; *(uint32_t*)(fh+10) = 54;
            fwrite(fh, 1, 14, f);
            uint8_t dh[40] = {};
            *(uint32_t*)(dh+0) = 40;
            *(int32_t*)(dh+4) = w;
            *(int32_t*)(dh+8) = -(int32_t)h;
            *(uint16_t*)(dh+12) = 1; *(uint16_t*)(dh+14) = 32;
            *(uint32_t*)(dh+20) = pixelDataSize;
            fwrite(dh, 1, 40, f);
            fwrite(g_icd.framePixels.data(), 1, w * h * 4, f);
            fclose(f);
        }
    }
#endif

    return VK_SUCCESS;
}

// --- Functions DXVK needs during init ---

static VkResult VKAPI_CALL icd_vkEnumerateInstanceLayerProperties(uint32_t* pCount, VkLayerProperties*) {
    *pCount = 0;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL icd_vkEnumerateInstanceVersion(uint32_t* pVersion) {
    *pVersion = VK_API_VERSION_1_3;
    return VK_SUCCESS;
}

static PFN_vkVoidFunction VKAPI_CALL icd_vkGetDeviceProcAddr(VkDevice device, const char* pName);

// --- Resource creation stubs (forward to encoder) ---

static VkResult VKAPI_CALL icd_vkCreateImageView(
    VkDevice, const VkImageViewCreateInfo* pInfo, const VkAllocationCallbacks*, VkImageView* p)
{
    uint64_t id = g_icd.handles.alloc();
    *p = (VkImageView)id;
    g_icd.imageViewToImage[id] = (uint64_t)pInfo->image;
    g_icd.encoder.cmdCreateImageView(1, id, pInfo->flags, (uint64_t)pInfo->image,
        pInfo->viewType, pInfo->format,
        pInfo->components.r, pInfo->components.g, pInfo->components.b, pInfo->components.a,
        pInfo->subresourceRange.aspectMask, pInfo->subresourceRange.baseMipLevel,
        pInfo->subresourceRange.levelCount, pInfo->subresourceRange.baseArrayLayer,
        pInfo->subresourceRange.layerCount);
    return VK_SUCCESS;
}

static void VKAPI_CALL icd_vkDestroyImageView(VkDevice, VkImageView v, const VkAllocationCallbacks*) { g_icd.encoder.cmdDestroyImageView(1, (uint64_t)v); }

static VkResult VKAPI_CALL icd_vkCreateRenderPass(
    VkDevice, const VkRenderPassCreateInfo* pInfo, const VkAllocationCallbacks*, VkRenderPass* p)
{
    uint64_t id = g_icd.handles.alloc();
    *p = (VkRenderPass)id;

    std::vector<uint32_t> fmts, loads, stores, initLayouts, finalLayouts;
    for (uint32_t i = 0; i < pInfo->attachmentCount; i++) {
        fmts.push_back(pInfo->pAttachments[i].format);
        loads.push_back(pInfo->pAttachments[i].loadOp);
        stores.push_back(pInfo->pAttachments[i].storeOp);
        initLayouts.push_back(pInfo->pAttachments[i].initialLayout);
        finalLayouts.push_back(pInfo->pAttachments[i].finalLayout);
    }
    g_icd.encoder.cmdCreateRenderPass(1, id, pInfo->attachmentCount,
        fmts.data(), loads.data(), stores.data(), initLayouts.data(), finalLayouts.data());
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL icd_vkCreateRenderPass2(VkDevice d, const VkRenderPassCreateInfo2* pInfo,
    const VkAllocationCallbacks* a, VkRenderPass* p)
{
    // Simplified: treat like CreateRenderPass
    uint64_t id = g_icd.handles.alloc();
    *p = (VkRenderPass)id;

    std::vector<uint32_t> fmts, loads, stores, initLayouts, finalLayouts;
    for (uint32_t i = 0; i < pInfo->attachmentCount; i++) {
        fmts.push_back(pInfo->pAttachments[i].format);
        loads.push_back(pInfo->pAttachments[i].loadOp);
        stores.push_back(pInfo->pAttachments[i].storeOp);
        initLayouts.push_back(pInfo->pAttachments[i].initialLayout);
        finalLayouts.push_back(pInfo->pAttachments[i].finalLayout);
    }
    g_icd.encoder.cmdCreateRenderPass(1, id, pInfo->attachmentCount,
        fmts.data(), loads.data(), stores.data(), initLayouts.data(), finalLayouts.data());
    return VK_SUCCESS;
}

static void VKAPI_CALL icd_vkDestroyRenderPass(VkDevice, VkRenderPass v, const VkAllocationCallbacks*) { g_icd.encoder.cmdDestroyRenderPass(1, (uint64_t)v); }

static VkResult VKAPI_CALL icd_vkCreateShaderModule(
    VkDevice, const VkShaderModuleCreateInfo* pInfo, const VkAllocationCallbacks*, VkShaderModule* p)
{
    uint64_t id = g_icd.handles.alloc();
    *p = (VkShaderModule)id;
    g_icd.encoder.cmdCreateShaderModule(1, id,
        pInfo->pCode, pInfo->codeSize);
    return VK_SUCCESS;
}

static void VKAPI_CALL icd_vkDestroyShaderModule(VkDevice, VkShaderModule v, const VkAllocationCallbacks*) { g_icd.encoder.cmdDestroyShaderModule(1, (uint64_t)v); }

static VkResult VKAPI_CALL icd_vkCreatePipelineLayout(
    VkDevice, const VkPipelineLayoutCreateInfo* pInfo, const VkAllocationCallbacks*, VkPipelineLayout* p)
{
    uint64_t id = g_icd.handles.alloc();
    *p = (VkPipelineLayout)id;

    std::vector<uint64_t> setLayoutIds(pInfo->setLayoutCount);
    for (uint32_t i = 0; i < pInfo->setLayoutCount; i++)
        setLayoutIds[i] = (uint64_t)pInfo->pSetLayouts[i];

    // Pack push constant ranges as flat u32 array (stageFlags, offset, size per range)
    std::vector<uint32_t> pushData(pInfo->pushConstantRangeCount * 3);
    for (uint32_t i = 0; i < pInfo->pushConstantRangeCount; i++) {
        pushData[i*3+0] = pInfo->pPushConstantRanges[i].stageFlags;
        pushData[i*3+1] = pInfo->pPushConstantRanges[i].offset;
        pushData[i*3+2] = pInfo->pPushConstantRanges[i].size;
    }
    g_icd.encoder.cmdCreatePipelineLayout(1, id,
        pInfo->setLayoutCount, setLayoutIds.data(),
        pInfo->pushConstantRangeCount, pushData.data());
    return VK_SUCCESS;
}

static void VKAPI_CALL icd_vkDestroyPipelineLayout(VkDevice, VkPipelineLayout v, const VkAllocationCallbacks*) { g_icd.encoder.cmdDestroyPipelineLayout(1, (uint64_t)v); }

// Helper: find VkShaderModuleCreateInfo in pNext chain (used when shader_module_identifier is active)
static const VkShaderModuleCreateInfo* findShaderModuleCreateInfo(const void* pNext) {
    auto* base = reinterpret_cast<const VkBaseInStructure*>(pNext);
    while (base) {
        if (base->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO)
            return reinterpret_cast<const VkShaderModuleCreateInfo*>(base);
        base = base->pNext;
    }
    return nullptr;
}

static VkResult VKAPI_CALL icd_vkCreateGraphicsPipelines(
    VkDevice, VkPipelineCache, uint32_t count, const VkGraphicsPipelineCreateInfo* pInfos,
    const VkAllocationCallbacks*, VkPipeline* pPipelines)
{
    for (uint32_t i = 0; i < count; i++) {
        uint64_t id = g_icd.handles.alloc();
        pPipelines[i] = (VkPipeline)id;

        fprintf(stderr, "[ICD] CreatePipeline: id=%llu flags=0x%x stages=%u rp=%llu pVIS=%p pRast=%p pBlend=%p\n",
                (unsigned long long)id, pInfos[i].flags, pInfos[i].stageCount,
                (unsigned long long)(uint64_t)pInfos[i].renderPass,
                (void*)pInfos[i].pVertexInputState, (void*)pInfos[i].pRasterizationState,
                (void*)pInfos[i].pColorBlendState);

        uint64_t vertMod = 0, fragMod = 0;
        if (pInfos[i].pStages) {
            for (uint32_t s = 0; s < pInfos[i].stageCount; s++) {
                uint64_t mod = (uint64_t)pInfos[i].pStages[s].module;

                // DXVK with graphics_pipeline_library may pass SPIR-V via pNext
                // even when module is non-null (the module may be an empty placeholder).
                // Always check pNext for real SPIR-V code.
                auto* smci = findShaderModuleCreateInfo(pInfos[i].pStages[s].pNext);
                fprintf(stderr, "[ICD] Pipeline stage %u: mod=%llu pNext=%p smci=%p codeSize=%zu\n",
                        s, (unsigned long long)mod, pInfos[i].pStages[s].pNext,
                        (void*)smci, smci ? smci->codeSize : 0);
                if (smci && smci->codeSize > 4) {
                    // pNext has real SPIR-V — create a proper shader module
                    mod = g_icd.handles.alloc();
                    g_icd.encoder.cmdCreateShaderModule(1, mod, smci->pCode, smci->codeSize);
                } else if (!mod) {
                    // No module and no pNext SPIR-V — nothing we can do
                }

                if (pInfos[i].pStages[s].stage == VK_SHADER_STAGE_VERTEX_BIT)
                    vertMod = mod;
                if (pInfos[i].pStages[s].stage == VK_SHADER_STAGE_FRAGMENT_BIT)
                    fragMod = mod;
            }
        }

        uint32_t w = 800, h = 600;
        if (pInfos[i].pViewportState && pInfos[i].pViewportState->pViewports) {
            w = (uint32_t)pInfos[i].pViewportState->pViewports[0].width;
            h = (uint32_t)pInfos[i].pViewportState->pViewports[0].height;
        }

        // Extract color/depth attachment format for dynamic rendering (renderPass == null)
        uint32_t colorFmt = 0;
        uint32_t depthFmt = 0;
        if (!pInfos[i].renderPass) {
            // Walk pNext for VkPipelineRenderingCreateInfo
            auto* base = reinterpret_cast<const VkBaseInStructure*>(pInfos[i].pNext);
            while (base) {
                if (base->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) {
                    auto* pri = reinterpret_cast<const VkPipelineRenderingCreateInfo*>(base);
                    if (pri->colorAttachmentCount > 0)
                        colorFmt = pri->pColorAttachmentFormats[0];
                    depthFmt = pri->depthAttachmentFormat;
                    break;
                }
                base = base->pNext;
            }
            if (!colorFmt) colorFmt = VK_FORMAT_B8G8R8A8_SRGB; // fallback
        }

        // Extract vertex input state
        std::vector<VnEncoder::VertexBinding> vtxBindings;
        std::vector<VnEncoder::VertexAttribute> vtxAttrs;
        if (pInfos[i].pVertexInputState) {
            auto* vis = pInfos[i].pVertexInputState;
            for (uint32_t b = 0; b < vis->vertexBindingDescriptionCount; b++) {
                auto& vb = vis->pVertexBindingDescriptions[b];
                vtxBindings.push_back({vb.binding, vb.stride, (uint32_t)vb.inputRate});
            }
            for (uint32_t a = 0; a < vis->vertexAttributeDescriptionCount; a++) {
                auto& va = vis->pVertexAttributeDescriptions[a];
                vtxAttrs.push_back({va.location, va.binding, (uint32_t)va.format, va.offset});
            }
        }

        // Extract blend state from first color attachment
        VnEncoder::BlendAttachment blendAtt{};
        const VnEncoder::BlendAttachment* pBlend = nullptr;
        if (pInfos[i].pColorBlendState && pInfos[i].pColorBlendState->attachmentCount > 0) {
            auto& att = pInfos[i].pColorBlendState->pAttachments[0];
            blendAtt.blendEnable = att.blendEnable;
            blendAtt.srcColorBlendFactor = att.srcColorBlendFactor;
            blendAtt.dstColorBlendFactor = att.dstColorBlendFactor;
            blendAtt.colorBlendOp = att.colorBlendOp;
            blendAtt.srcAlphaBlendFactor = att.srcAlphaBlendFactor;
            blendAtt.dstAlphaBlendFactor = att.dstAlphaBlendFactor;
            blendAtt.alphaBlendOp = att.alphaBlendOp;
            blendAtt.colorWriteMask = att.colorWriteMask;
            pBlend = &blendAtt;
        }

        std::vector<uint32_t> dynamicStates;
        if (pInfos[i].pDynamicState) {
            auto* dyn = pInfos[i].pDynamicState;
            for (uint32_t d = 0; d < dyn->dynamicStateCount; d++)
                dynamicStates.push_back((uint32_t)dyn->pDynamicStates[d]);
        }

        VnEncoder::PipelineState pipelineState{};
        pipelineState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        pipelineState.polygonMode = VK_POLYGON_MODE_FILL;
        pipelineState.cullMode = VK_CULL_MODE_NONE;
        pipelineState.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        pipelineState.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        if (pInfos[i].pInputAssemblyState) {
            auto* ia = pInfos[i].pInputAssemblyState;
            pipelineState.topology = (uint32_t)ia->topology;
            pipelineState.primitiveRestartEnable = ia->primitiveRestartEnable;
        }
        if (pInfos[i].pRasterizationState) {
            auto* rs = pInfos[i].pRasterizationState;
            pipelineState.depthClampEnable = rs->depthClampEnable;
            pipelineState.rasterizerDiscardEnable = rs->rasterizerDiscardEnable;
            pipelineState.polygonMode = (uint32_t)rs->polygonMode;
            pipelineState.cullMode = (uint32_t)rs->cullMode;
            pipelineState.frontFace = (uint32_t)rs->frontFace;
            pipelineState.depthBiasEnable = rs->depthBiasEnable;
        }
        if (pInfos[i].pDepthStencilState) {
            auto* ds = pInfos[i].pDepthStencilState;
            pipelineState.depthTestEnable = ds->depthTestEnable;
            pipelineState.depthWriteEnable = ds->depthWriteEnable;
            pipelineState.depthCompareOp = (uint32_t)ds->depthCompareOp;
            pipelineState.depthBoundsTestEnable = ds->depthBoundsTestEnable;
            pipelineState.stencilTestEnable = ds->stencilTestEnable;
        }
        pipelineState.dynamicStateCount = (uint32_t)dynamicStates.size();
        pipelineState.dynamicStates = dynamicStates.data();

        g_icd.encoder.cmdCreateGraphicsPipeline(1, id,
            (uint64_t)pInfos[i].renderPass, (uint64_t)pInfos[i].layout,
            vertMod, fragMod, w, h, colorFmt,
            (uint32_t)vtxBindings.size(), vtxBindings.data(),
            (uint32_t)vtxAttrs.size(), vtxAttrs.data(), depthFmt, pBlend,
            &pipelineState);
    }
    return VK_SUCCESS;
}

static void VKAPI_CALL icd_vkDestroyPipeline(VkDevice, VkPipeline v, const VkAllocationCallbacks*) { g_icd.encoder.cmdDestroyPipeline(1, (uint64_t)v); }

static VkResult VKAPI_CALL icd_vkCreateFramebuffer(
    VkDevice, const VkFramebufferCreateInfo* pInfo, const VkAllocationCallbacks*, VkFramebuffer* p)
{
    uint64_t id = g_icd.handles.alloc();
    *p = (VkFramebuffer)id;
    uint64_t ivId = pInfo->attachmentCount > 0 ? (uint64_t)pInfo->pAttachments[0] : 0;
    g_icd.encoder.cmdCreateFramebuffer(1, id,
        (uint64_t)pInfo->renderPass, ivId, pInfo->width, pInfo->height);
    return VK_SUCCESS;
}

static void VKAPI_CALL icd_vkDestroyFramebuffer(VkDevice, VkFramebuffer v, const VkAllocationCallbacks*) { g_icd.encoder.cmdDestroyFramebuffer(1, (uint64_t)v); }

static VkResult VKAPI_CALL icd_vkCreateCommandPool(
    VkDevice, const VkCommandPoolCreateInfo* pInfo, const VkAllocationCallbacks*, VkCommandPool* p)
{
    uint64_t id = g_icd.handles.alloc();
    *p = (VkCommandPool)id;
    g_icd.encoder.cmdCreateCommandPool(1, id, pInfo->flags, pInfo->queueFamilyIndex);
    return VK_SUCCESS;
}

static void VKAPI_CALL icd_vkDestroyCommandPool(VkDevice, VkCommandPool v, const VkAllocationCallbacks*) { g_icd.encoder.cmdDestroyCommandPool(1, (uint64_t)v); }
static VkResult VKAPI_CALL icd_vkResetCommandPool(VkDevice, VkCommandPool, VkCommandPoolResetFlags) { return VK_SUCCESS; }

static VkResult VKAPI_CALL icd_vkAllocateCommandBuffers(
    VkDevice, const VkCommandBufferAllocateInfo* pInfo, VkCommandBuffer* pCBs)
{
    for (uint32_t i = 0; i < pInfo->commandBufferCount; i++) {
        uint64_t id = g_icd.handles.alloc();
        auto* h = reinterpret_cast<DispatchableHandle*>(makeDispatchable(id));
        pCBs[i] = reinterpret_cast<VkCommandBuffer>(h);
        fprintf(stderr, "[ICD] AllocCB: id=%llu handle=%p pool=%llu\n",
                (unsigned long long)id, (void*)h, (unsigned long long)(uint64_t)pInfo->commandPool);
        g_icd.encoder.cmdAllocateCommandBuffers(1, (uint64_t)pInfo->commandPool, id);
    }
    return VK_SUCCESS;
}

static void VKAPI_CALL icd_vkFreeCommandBuffers(VkDevice, VkCommandPool, uint32_t count, const VkCommandBuffer* pCBs) {
    for (uint32_t i = 0; i < count; i++)
        if (pCBs[i]) delete reinterpret_cast<DispatchableHandle*>(pCBs[i]);
}

static VkResult VKAPI_CALL icd_vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo*) {
    uint64_t id = toId(cb);
    fprintf(stderr, "[ICD] BeginCB: handle=%p id=%llu\n", (void*)cb, (unsigned long long)id);
    g_icd.encoder.cmdBeginCommandBuffer(id);
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL icd_vkEndCommandBuffer(VkCommandBuffer cb) {
    g_icd.encoder.cmdEndCommandBuffer(toId(cb));
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL icd_vkResetCommandBuffer(VkCommandBuffer, VkCommandBufferResetFlags) { return VK_SUCCESS; }

// --- Command recording ---

static void VKAPI_CALL icd_vkCmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo* pInfo, VkSubpassContents) {
    float cr = 0, cg = 0, cb_ = 0, ca = 1;
    if (pInfo->clearValueCount > 0) {
        cr = pInfo->pClearValues[0].color.float32[0];
        cg = pInfo->pClearValues[0].color.float32[1];
        cb_ = pInfo->pClearValues[0].color.float32[2];
        ca = pInfo->pClearValues[0].color.float32[3];
    }
    g_icd.encoder.cmdBeginRenderPass(toId(cb),
        (uint64_t)pInfo->renderPass, (uint64_t)pInfo->framebuffer,
        pInfo->renderArea.extent.width, pInfo->renderArea.extent.height,
        cr, cg, cb_, ca);
}

static void VKAPI_CALL icd_vkCmdBeginRenderPass2(VkCommandBuffer cb, const VkRenderPassBeginInfo* pInfo, const VkSubpassBeginInfo*) {
    icd_vkCmdBeginRenderPass(cb, pInfo, VK_SUBPASS_CONTENTS_INLINE);
}

static void VKAPI_CALL icd_vkCmdEndRenderPass(VkCommandBuffer cb) {
    g_icd.encoder.cmdEndRenderPass(toId(cb));
}

static void VKAPI_CALL icd_vkCmdEndRenderPass2(VkCommandBuffer cb, const VkSubpassEndInfo*) {
    icd_vkCmdEndRenderPass(cb);
}

static void VKAPI_CALL icd_vkCmdNextSubpass(VkCommandBuffer, VkSubpassContents) {}
static void VKAPI_CALL icd_vkCmdNextSubpass2(VkCommandBuffer, const VkSubpassBeginInfo*, const VkSubpassEndInfo*) {}

static void VKAPI_CALL icd_vkCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bindPoint, VkPipeline pipeline) {
    g_icd.encoder.cmdBindPipeline(toId(cb), static_cast<uint32_t>(bindPoint), (uint64_t)pipeline);
}

static void VKAPI_CALL icd_vkCmdSetViewport(VkCommandBuffer cb, uint32_t, uint32_t count, const VkViewport* vps) {
    if (count > 0)
        g_icd.encoder.cmdSetViewport(toId(cb), vps[0].x, vps[0].y, vps[0].width, vps[0].height, vps[0].minDepth, vps[0].maxDepth);
}

static void VKAPI_CALL icd_vkCmdSetScissor(VkCommandBuffer cb, uint32_t, uint32_t count, const VkRect2D* rects) {
    if (count > 0)
        g_icd.encoder.cmdSetScissor(toId(cb), rects[0].offset.x, rects[0].offset.y, rects[0].extent.width, rects[0].extent.height);
}

static void VKAPI_CALL icd_vkCmdDraw(VkCommandBuffer cb, uint32_t vertexCount, uint32_t instanceCount,
    uint32_t firstVertex, uint32_t firstInstance)
{
    g_icd.encoder.cmdDraw(toId(cb), vertexCount, instanceCount, firstVertex, firstInstance);
}

// --- Sync ---

static VkResult VKAPI_CALL icd_vkCreateSemaphore(VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*, VkSemaphore* p) {
    uint64_t id = g_icd.handles.alloc();
    *p = (VkSemaphore)id;
    g_icd.encoder.cmdCreateSemaphore(1, id);
    return VK_SUCCESS;
}

static void VKAPI_CALL icd_vkDestroySemaphore(VkDevice, VkSemaphore v, const VkAllocationCallbacks*) { g_icd.encoder.cmdDestroySemaphore(1, (uint64_t)v); }

static VkResult VKAPI_CALL icd_vkCreateFence(VkDevice, const VkFenceCreateInfo* pInfo, const VkAllocationCallbacks*, VkFence* p) {
    uint64_t id = g_icd.handles.alloc();
    *p = (VkFence)id;
    g_icd.encoder.cmdCreateFence(1, id, pInfo ? pInfo->flags : 0);
    return VK_SUCCESS;
}

static void VKAPI_CALL icd_vkDestroyFence(VkDevice, VkFence v, const VkAllocationCallbacks*) { g_icd.encoder.cmdDestroyFence(1, (uint64_t)v); }
static VkResult VKAPI_CALL icd_vkWaitForFences(VkDevice, uint32_t count, const VkFence* p, VkBool32 waitAll, uint64_t timeout) {
    // Encode WaitForFences to be included in the next QueuePresent batch.
    // Host handles actual GPU fence synchronization; guest fences are virtual.
    // Returning VK_SUCCESS immediately is safe: guest resources (encoder buffers)
    // are already in the TCP batch and the host processes them in order.
    if (count > 0 && p)
        g_icd.encoder.cmdWaitForFences(1, count, reinterpret_cast<const uint64_t*>(p), waitAll, timeout);
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL icd_vkResetFences(VkDevice, uint32_t count, const VkFence* p) {
    if (count > 0 && p) {
        g_icd.encoder.cmdResetFences(1, count, reinterpret_cast<const uint64_t*>(p));
    }
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL icd_vkGetFenceStatus(VkDevice, VkFence) { return VK_SUCCESS; }

// --- Queue submit ---

static VkResult VKAPI_CALL icd_vkQueueSubmit(VkQueue q, uint32_t count, const VkSubmitInfo* pSubmits, VkFence fence) {
    // Hold encoder lock for the entire flush+submit sequence so all WriteMemory
    // commands appear before QueueSubmit in the stream, with no interleaving
    // from other DXVK threads.
    ENC_LOCK;
    g_icd.flushMappedMemory();
    for (uint32_t i = 0; i < count; i++) {
        uint32_t cbCount = pSubmits[i].commandBufferCount;
        uint64_t waitSem = pSubmits[i].waitSemaphoreCount > 0 ? (uint64_t)pSubmits[i].pWaitSemaphores[0] : 0;
        uint64_t sigSem = pSubmits[i].signalSemaphoreCount > 0 ? (uint64_t)pSubmits[i].pSignalSemaphores[0] : 0;
        bool isLast = (i == count - 1);
        if (cbCount == 0) {
            // No command buffers — still need to forward semaphores and fence
            uint64_t f = isLast ? (uint64_t)fence : 0;
            if (waitSem || sigSem || f) {
                g_icd.encoder.cmdQueueSubmit(toId(q), 0, waitSem, sigSem, f);
            }
        } else {
            for (uint32_t j = 0; j < cbCount; j++) {
                uint64_t cbId = toId(pSubmits[i].pCommandBuffers[j]);
                uint64_t ws = (j == 0) ? waitSem : 0;
                uint64_t ss = (j == cbCount - 1) ? sigSem : 0;
                uint64_t f  = (isLast && j == cbCount - 1) ? (uint64_t)fence : 0;
                g_icd.encoder.cmdQueueSubmit(toId(q), cbId, ws, ss, f);
            }
        }
    }
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL icd_vkQueueSubmit2(VkQueue q, uint32_t count, const VkSubmitInfo2* pSubmits, VkFence fence) {
    ENC_LOCK;
    g_icd.flushMappedMemory();
    for (uint32_t i = 0; i < count; i++) {
        uint32_t cbCount = pSubmits[i].commandBufferInfoCount;
        uint64_t waitSem = pSubmits[i].waitSemaphoreInfoCount > 0 ? (uint64_t)pSubmits[i].pWaitSemaphoreInfos[0].semaphore : 0;
        uint64_t sigSem = pSubmits[i].signalSemaphoreInfoCount > 0 ? (uint64_t)pSubmits[i].pSignalSemaphoreInfos[0].semaphore : 0;
        if (waitSem > 10000 || sigSem > 10000) {
            // BAD SEM still logged — this catches real bugs
            icdDbg((std::string("QueueSubmit2 BAD SEM: waitSem=") + std::to_string(waitSem)
                + " sigSem=" + std::to_string(sigSem)
                + " sizeof(VkSemaphoreSubmitInfo)=" + std::to_string(sizeof(VkSemaphoreSubmitInfo))
                + " offsetof(sem)=" + std::to_string(offsetof(VkSemaphoreSubmitInfo, semaphore))).c_str());
        }
        bool isLast = (i == count - 1);
        if (cbCount == 0) {
            uint64_t f = isLast ? (uint64_t)fence : 0;
            if (waitSem || sigSem || f) {
                g_icd.encoder.cmdQueueSubmit(toId(q), 0, waitSem, sigSem, f);
            }
        } else {
            for (uint32_t j = 0; j < cbCount; j++) {
                uint64_t cbId = toId(pSubmits[i].pCommandBufferInfos[j].commandBuffer);
                uint64_t ws = (j == 0) ? waitSem : 0;
                uint64_t ss = (j == cbCount - 1) ? sigSem : 0;
                uint64_t f  = (isLast && j == cbCount - 1) ? (uint64_t)fence : 0;
                g_icd.encoder.cmdQueueSubmit(toId(q), cbId, ws, ss, f);
            }
        }
    }
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL icd_vkQueueWaitIdle(VkQueue) { return VK_SUCCESS; }

// --- Memory ---

static VkResult VKAPI_CALL icd_vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo* pInfo, const VkAllocationCallbacks*, VkDeviceMemory* p) {
    uint64_t id = g_icd.handles.alloc();
    *p = (VkDeviceMemory)id;
    // Allocate shadow with VirtualAlloc for page-level protection support
    VkDeviceSize allocSize = pInfo->allocationSize;
    void* shadow = VirtualAlloc(nullptr, (SIZE_T)allocSize,
        MEM_COMMIT | MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (!shadow) shadow = VirtualAlloc(nullptr, (SIZE_T)allocSize,
        MEM_COMMIT | MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE); // retry
    g_icd.memoryShadows[id] = {shadow, allocSize};
    g_icd.encoder.cmdAllocateMemory(1, id, pInfo->allocationSize, pInfo->memoryTypeIndex);
    return VK_SUCCESS;
}
static void VKAPI_CALL icd_vkFreeMemory(VkDevice, VkDeviceMemory mem, const VkAllocationCallbacks*) {
    uint64_t id = (uint64_t)mem;
    // Free shadow buffer + remove mapped regions under lock
    {
        std::lock_guard<std::mutex> lock(g_icd.mappedMutex);
        auto it = g_icd.memoryShadows.find(id);
        if (it != g_icd.memoryShadows.end()) {
            if (it->second.ptr) VirtualFree(it->second.ptr, 0, MEM_RELEASE);
            g_icd.memoryShadows.erase(it);
        }
        g_icd.mappedRegions.erase(
            std::remove_if(g_icd.mappedRegions.begin(), g_icd.mappedRegions.end(),
                            [id](const IcdState::MappedRegion& r) { return r.memoryId == id; }),
            g_icd.mappedRegions.end());
    }
    // Encoder call outside lock to avoid deadlock (encoder flush → flushMappedMemory → mappedMutex)
    g_icd.encoder.cmdFreeMemory(1, (uint64_t)mem);
}
static VkResult VKAPI_CALL icd_vkMapMemory(VkDevice, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags, void** ppData) {
    uint64_t memId = (uint64_t)memory;
    // Return a pointer into the per-memory shadow buffer (persistent mapping safe)
    auto it = g_icd.memoryShadows.find(memId);
    if (it == g_icd.memoryShadows.end()) {
        fprintf(stderr, "[ICD] MapMemory: no shadow for memory %llu!\n", memId);
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    VkDeviceSize actualSize = (size == VK_WHOLE_SIZE) ? (it->second.size - offset) : size;
    *ppData = (uint8_t*)it->second.ptr + offset;
    std::lock_guard<std::mutex> lock(g_icd.mappedMutex);
    g_icd.mappedRegions.push_back({memId, offset, actualSize, *ppData, false});
    static int mapLog = 0;
    if (mapLog++ < 5)
        icdDbg(("[ICD] MapMemory: mem=" + std::to_string(memId) + " off=" + std::to_string(offset) + " sz=" + std::to_string(actualSize) + " total_mapped=" + std::to_string(g_icd.mappedRegions.size())).c_str());
    return VK_SUCCESS;
}
// vkMapMemory2 / vkMapMemory2KHR (Vulkan 1.4 / VK_KHR_map_memory2)
static VkResult VKAPI_CALL icd_vkMapMemory2(VkDevice dev, const void* pMemoryMapInfo, void** ppData) {
    // VkMemoryMapInfoKHR: sType, pNext, flags, memory, offset, size
    struct MemMapInfo { VkStructureType sType; const void* pNext; VkMemoryMapFlags flags; VkDeviceMemory memory; VkDeviceSize offset; VkDeviceSize size; };
    auto* info = static_cast<const MemMapInfo*>(pMemoryMapInfo);
    static int mm2Log = 0;
    if (mm2Log++ < 5) {
        icdDbg(("[ICD] MapMemory2: mem=" + std::to_string((uint64_t)info->memory)
                + " off=" + std::to_string(info->offset) + " sz=" + std::to_string(info->size)
                + " flags=" + std::to_string(info->flags)
                + " sizeof=" + std::to_string(sizeof(MemMapInfo))).c_str());
    }
    return icd_vkMapMemory(dev, info->memory, info->offset, info->size, info->flags, ppData);
}

static void VKAPI_CALL icd_vkUnmapMemory(VkDevice, VkDeviceMemory memory) {
    uint64_t memId = (uint64_t)memory;
    std::lock_guard<std::mutex> lock(g_icd.mappedMutex);
    for (auto it = g_icd.mappedRegions.begin(); it != g_icd.mappedRegions.end(); ++it) {
        if (it->memoryId == memId) {
            // Send shadow data to host before removing the mapping record
            // (shadow buffer itself stays alive in memoryShadows — not freed here)
            g_icd.encoder.cmdWriteMemory(memId, it->offset, (uint32_t)it->size, it->ptr);
            g_icd.mappedRegions.erase(it);
            break;
        }
    }
}
static VkResult VKAPI_CALL icd_vkUnmapMemory2(VkDevice dev, const void* pMemoryUnmapInfo) {
    struct MemUnmapInfo { VkStructureType sType; const void* pNext; uint32_t flags; VkDeviceMemory memory; };
    auto* info = static_cast<const MemUnmapInfo*>(pMemoryUnmapInfo);
    icd_vkUnmapMemory(dev, info->memory);
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL icd_vkBindBufferMemory(VkDevice, VkBuffer buf, VkDeviceMemory mem, VkDeviceSize offset) {
    uint64_t bufId = (uint64_t)buf;
    g_icd.bufferBindings[bufId] = {(uint64_t)mem, offset};
    g_icd.encoder.cmdBindBufferMemory(1, bufId, (uint64_t)mem, offset);
    // If buffer needs BDA, flush now so Host auto-generates BDA in the response.
    // GetBDA (called next by DXVK) will just wait for the cache to be populated.
    if (g_icd.bdaNeedBuffers_.count(bufId)) {
        g_icd.sendBatch(false);
        g_icd.bdaNeedBuffers_.erase(bufId);
    }
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL icd_vkBindImageMemory(VkDevice, VkImage img, VkDeviceMemory mem, VkDeviceSize offset) {
    g_icd.encoder.cmdBindImageMemory(1, (uint64_t)img, (uint64_t)mem, offset);
    return VK_SUCCESS;
}
static void VKAPI_CALL icd_vkGetBufferMemoryRequirements(VkDevice, VkBuffer buf, VkMemoryRequirements* p) {
    uint64_t id = (uint64_t)buf;
    auto it = g_icd.bufferSizes.find(id);
    VkDeviceSize sz = (it != g_icd.bufferSizes.end()) ? it->second : 65536;
    // Align up to 256
    sz = (sz + 255) & ~(VkDeviceSize)255;
    p->size = sz; p->alignment = 256; p->memoryTypeBits = 0x3;
}
static void VKAPI_CALL icd_vkGetImageMemoryRequirements(VkDevice, VkImage, VkMemoryRequirements* p) {
    p->size = 4 * 1024 * 1024; p->alignment = 256; p->memoryTypeBits = 0x3;
}
static void VKAPI_CALL icd_vkGetBufferMemoryRequirements2(VkDevice, const VkBufferMemoryRequirementsInfo2* pInfo, VkMemoryRequirements2* p) {
    uint64_t id = pInfo ? (uint64_t)pInfo->buffer : 0;
    auto it = g_icd.bufferSizes.find(id);
    VkDeviceSize sz = (it != g_icd.bufferSizes.end()) ? it->second : 65536;
    sz = (sz + 255) & ~(VkDeviceSize)255;
    p->memoryRequirements.size = sz; p->memoryRequirements.alignment = 256; p->memoryRequirements.memoryTypeBits = 0x3;
}
static void VKAPI_CALL icd_vkGetImageMemoryRequirements2(VkDevice, const VkImageMemoryRequirementsInfo2*, VkMemoryRequirements2* p) {
    p->memoryRequirements.size = 4*1024*1024; p->memoryRequirements.alignment = 256; p->memoryRequirements.memoryTypeBits = 0x3;
}
// Vulkan 1.3 / VK_KHR_maintenance4: query memory requirements without creating objects.
// DXVK calls this at init to probe which memory types support each buffer usage flag.
// Returning valid results enables DXVK's global buffer sub-allocation (critical for perf).
static void VKAPI_CALL icd_vkGetDeviceBufferMemoryRequirements(VkDevice, const VkDeviceBufferMemoryRequirements* pInfo, VkMemoryRequirements2* p) {
    VkDeviceSize sz = pInfo->pCreateInfo->size;
    sz = (sz + 255) & ~(VkDeviceSize)255;
    p->sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    p->memoryRequirements.size = sz;
    p->memoryRequirements.alignment = 256;
    p->memoryRequirements.memoryTypeBits = 0x3;
}
static void VKAPI_CALL icd_vkGetDeviceImageMemoryRequirements(VkDevice, const VkDeviceImageMemoryRequirements*, VkMemoryRequirements2* p) {
    p->sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    p->memoryRequirements.size = 4*1024*1024;
    p->memoryRequirements.alignment = 256;
    p->memoryRequirements.memoryTypeBits = 0x3;
}

// --- Buffer / Image creation ---

static VkResult VKAPI_CALL icd_vkCreateBuffer(VkDevice, const VkBufferCreateInfo* pInfo, const VkAllocationCallbacks*, VkBuffer* p) {
    uint64_t id = g_icd.handles.alloc();
    *p = (VkBuffer)id;
    g_icd.bufferSizes[id] = pInfo->size;
    g_icd.encoder.cmdCreateBuffer(1, id, pInfo);
    // Track buffers that need BDA — BindBufferMemory will flush immediately
    if (pInfo->usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
        g_icd.bdaNeedBuffers_.insert(id);
    return VK_SUCCESS;
}
static void VKAPI_CALL icd_vkDestroyBuffer(VkDevice, VkBuffer v, const VkAllocationCallbacks*) {
    uint64_t id = (uint64_t)v;
    g_icd.bdaCache.erase(id);
    g_icd.bdaNeedBuffers_.erase(id);
    { std::lock_guard<std::mutex> lk(g_icd.bdaMutex_); g_icd.bdaRecorded_.erase(id); }
    g_icd.encoder.cmdDestroyBuffer(1, id);
}
static VkResult VKAPI_CALL icd_vkCreateImage(VkDevice, const VkImageCreateInfo* pInfo, const VkAllocationCallbacks*, VkImage* p) {
    uint64_t id = g_icd.handles.alloc();
    *p = (VkImage)id;
    g_icd.imageFormats[id] = pInfo->format;
    g_icd.encoder.cmdCreateImage(1, id,
        pInfo->imageType, pInfo->format,
        pInfo->extent.width, pInfo->extent.height, pInfo->extent.depth,
        pInfo->mipLevels, pInfo->arrayLayers, pInfo->samples,
        pInfo->tiling, pInfo->usage);
    return VK_SUCCESS;
}
static void VKAPI_CALL icd_vkDestroyImage(VkDevice, VkImage v, const VkAllocationCallbacks*) {
    g_icd.imageFormats.erase((uint64_t)v);
    g_icd.encoder.cmdDestroyImage(1, (uint64_t)v);
}

// --- Sampler / Descriptor ---

static VkResult VKAPI_CALL icd_vkCreateSampler(VkDevice, const VkSamplerCreateInfo* pInfo, const VkAllocationCallbacks*, VkSampler* p) {
    uint64_t id = g_icd.handles.alloc();
    *p = (VkSampler)id;
    g_icd.encoder.cmdCreateSampler(1, id, pInfo);
    return VK_SUCCESS;
}
static void VKAPI_CALL icd_vkDestroySampler(VkDevice, VkSampler v, const VkAllocationCallbacks*) { g_icd.encoder.cmdDestroySampler(1, (uint64_t)v); }

static VkResult VKAPI_CALL icd_vkCreateDescriptorSetLayout(
    VkDevice, const VkDescriptorSetLayoutCreateInfo* pInfo,
    const VkAllocationCallbacks*, VkDescriptorSetLayout* p)
{
    uint64_t id = g_icd.handles.alloc();
    *p = (VkDescriptorSetLayout)id;
    g_icd.encoder.cmdCreateDescriptorSetLayout(1, id,
        pInfo->bindingCount, pInfo->pBindings);
    return VK_SUCCESS;
}
static void VKAPI_CALL icd_vkDestroyDescriptorSetLayout(VkDevice, VkDescriptorSetLayout v, const VkAllocationCallbacks*) { g_icd.encoder.cmdDestroyDescriptorSetLayout(1, (uint64_t)v); }

static VkResult VKAPI_CALL icd_vkCreateDescriptorPool(VkDevice, const VkDescriptorPoolCreateInfo* pInfo, const VkAllocationCallbacks*, VkDescriptorPool* p) {
    uint64_t id = g_icd.handles.alloc();
    *p = (VkDescriptorPool)id;
    g_icd.encoder.cmdCreateDescriptorPool(1, id, pInfo->flags, pInfo->maxSets,
        pInfo->poolSizeCount, pInfo->pPoolSizes);
    return VK_SUCCESS;
}
static void VKAPI_CALL icd_vkDestroyDescriptorPool(VkDevice, VkDescriptorPool v, const VkAllocationCallbacks*) { g_icd.encoder.cmdDestroyDescriptorPool(1, (uint64_t)v); }
static VkResult VKAPI_CALL icd_vkResetDescriptorPool(VkDevice, VkDescriptorPool, VkDescriptorPoolResetFlags) { return VK_SUCCESS; }

static VkResult VKAPI_CALL icd_vkAllocateDescriptorSets(VkDevice, const VkDescriptorSetAllocateInfo* pInfo, VkDescriptorSet* p) {
    std::vector<uint64_t> layoutIds(pInfo->descriptorSetCount);
    std::vector<uint64_t> setIds(pInfo->descriptorSetCount);
    for (uint32_t i = 0; i < pInfo->descriptorSetCount; i++) {
        setIds[i] = g_icd.handles.alloc();
        p[i] = (VkDescriptorSet)setIds[i];
        layoutIds[i] = (uint64_t)pInfo->pSetLayouts[i];
    }
    fprintf(stderr, "[ICD] AllocateDescriptorSets: pool=%llu count=%u\n",
            (unsigned long long)(uint64_t)pInfo->descriptorPool, pInfo->descriptorSetCount);
    g_icd.encoder.cmdAllocateDescriptorSets(1, (uint64_t)pInfo->descriptorPool,
        pInfo->descriptorSetCount, layoutIds.data(), setIds.data());
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL icd_vkFreeDescriptorSets(VkDevice, VkDescriptorPool, uint32_t, const VkDescriptorSet*) { return VK_SUCCESS; }
static void VKAPI_CALL icd_vkUpdateDescriptorSets(VkDevice, uint32_t writeCount, const VkWriteDescriptorSet* pWrites, uint32_t, const VkCopyDescriptorSet*) {
    if (writeCount > 0 && pWrites) {
        // Flush buffer data for any buffer descriptors before encoding
        for (uint32_t i = 0; i < writeCount; i++) {
            if (pWrites[i].pBufferInfo) {
                for (uint32_t j = 0; j < pWrites[i].descriptorCount; j++) {
                    auto& bi = pWrites[i].pBufferInfo[j];
                    if (bi.buffer)
                        g_icd.flushBufferRange((uint64_t)bi.buffer, bi.offset, bi.range);
                }
            }
        }
        g_icd.encoder.cmdUpdateDescriptorSets(1, writeCount, pWrites);
    }
}

// --- Pipeline cache ---

static VkResult VKAPI_CALL icd_vkCreatePipelineCache(VkDevice, const VkPipelineCacheCreateInfo*, const VkAllocationCallbacks*, VkPipelineCache* p) {
    *p = (VkPipelineCache)g_icd.handles.alloc(); return VK_SUCCESS;
}
static void VKAPI_CALL icd_vkDestroyPipelineCache(VkDevice, VkPipelineCache, const VkAllocationCallbacks*) {}

// --- Various command stubs ---

static void VKAPI_CALL icd_vkCmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bindPoint, VkPipelineLayout layout,
    uint32_t firstSet, uint32_t setCount, const VkDescriptorSet* pSets, uint32_t dynOffCount, const uint32_t* pDynOff) {
    std::vector<uint64_t> setIds(setCount);
    for (uint32_t i = 0; i < setCount; i++) setIds[i] = (uint64_t)pSets[i];
    g_icd.encoder.cmdBindDescriptorSets(toId(cb), bindPoint, (uint64_t)layout,
        firstSet, setCount, setIds.data(), dynOffCount, pDynOff);
}
static void VKAPI_CALL icd_vkCmdBindVertexBuffers(VkCommandBuffer cb, uint32_t firstBinding, uint32_t bindingCount,
    const VkBuffer* pBuffers, const VkDeviceSize* pOffsets) {
    std::vector<uint64_t> ids(bindingCount), offs(bindingCount);
    for (uint32_t i = 0; i < bindingCount; i++) {
        ids[i] = (uint64_t)pBuffers[i]; offs[i] = pOffsets[i];
    }
    // No explicit flush: dirty tracking (GetWriteWatch in flushMappedMemory at QueueSubmit time)
    // captures all writes. WriteMemory is sent before BatchSubmit, so GPU reads correct data.
    g_icd.encoder.cmdBindVertexBuffers(toId(cb), firstBinding, bindingCount,
        ids.data(), offs.data(), nullptr, nullptr);
}
static void VKAPI_CALL icd_vkCmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize offset, VkIndexType indexType) {
    // No explicit flush: dirty tracking handles this.
    g_icd.encoder.cmdBindIndexBuffer(toId(cb), (uint64_t)buf, offset, indexType);
}
static void VKAPI_CALL icd_vkCmdBindIndexBuffer2(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize offset, VkDeviceSize size, VkIndexType indexType) {
    // No explicit flush: dirty tracking handles this.
    g_icd.encoder.cmdBindIndexBuffer(toId(cb), (uint64_t)buf, offset, indexType);
}
static void VKAPI_CALL icd_vkCmdDrawIndexed(VkCommandBuffer cb, uint32_t indexCount, uint32_t instanceCount,
    uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
    g_icd.encoder.cmdDrawIndexed(toId(cb), indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}
static void VKAPI_CALL icd_vkCmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t regionCount, const VkBufferCopy* pRegions) {
    // Flush source buffer memory for each region (staging buffer data → host)
    for (uint32_t i = 0; i < regionCount; i++)
        g_icd.flushBufferRange((uint64_t)src, pRegions[i].srcOffset, pRegions[i].size);
    std::vector<uint64_t> srcOffs(regionCount), dstOffs(regionCount), sizes(regionCount);
    for (uint32_t i = 0; i < regionCount; i++) {
        srcOffs[i] = pRegions[i].srcOffset; dstOffs[i] = pRegions[i].dstOffset; sizes[i] = pRegions[i].size;
    }
    g_icd.encoder.cmdCopyBuffer(toId(cb), (uint64_t)src, (uint64_t)dst, regionCount,
        srcOffs.data(), dstOffs.data(), sizes.data());
}
// Vulkan 1.3 vkCmdCopyBuffer2 — wraps to cmdCopyBuffer
static void VKAPI_CALL icd_vkCmdCopyBuffer2(VkCommandBuffer cb, const void* pCopyInfo) {
    struct CopyBufferInfo2 {
        VkStructureType sType; const void* pNext;
        VkBuffer srcBuffer, dstBuffer;
        uint32_t regionCount;
        const VkBufferCopy2* pRegions;
    };
    auto* info = static_cast<const CopyBufferInfo2*>(pCopyInfo);
    // Flush source buffer memory for each region
    for (uint32_t i = 0; i < info->regionCount; i++)
        g_icd.flushBufferRange((uint64_t)info->srcBuffer, info->pRegions[i].srcOffset, info->pRegions[i].size);
    std::vector<uint64_t> srcOffs(info->regionCount), dstOffs(info->regionCount), sizes(info->regionCount);
    for (uint32_t i = 0; i < info->regionCount; i++) {
        srcOffs[i] = info->pRegions[i].srcOffset;
        dstOffs[i] = info->pRegions[i].dstOffset;
        sizes[i] = info->pRegions[i].size;
    }
    g_icd.encoder.cmdCopyBuffer(toId(cb), (uint64_t)info->srcBuffer, (uint64_t)info->dstBuffer,
        info->regionCount, srcOffs.data(), dstOffs.data(), sizes.data());
}
// Vulkan 1.3 vkCmdCopyBufferToImage2 — wraps to cmdCopyBufferToImage

// Returns bytes per texel for common formats; 0 = unknown (caller should fall back).
static uint32_t formatBpp(VkFormat fmt) {
    switch (fmt) {
    // 1 bpp
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM:
    case VK_FORMAT_R8_UINT:  case VK_FORMAT_R8_SINT:
    case VK_FORMAT_R8_SRGB:
        return 1;
    // 2 bpp
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM:
    case VK_FORMAT_R8G8_UINT:  case VK_FORMAT_R8G8_SINT:
    case VK_FORMAT_R8G8_SRGB:
    case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SNORM:
    case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT:
    case VK_FORMAT_D16_UNORM:
        return 2;
    // 3 bpp
    case VK_FORMAT_R8G8B8_UNORM: case VK_FORMAT_B8G8R8_UNORM:
    case VK_FORMAT_R8G8B8_SRGB:  case VK_FORMAT_B8G8R8_SRGB:
        return 3;
    // 4 bpp
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:  case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32: case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
    case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT:
    case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
        return 4;
    // 8 bpp
    case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_UINT:  case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32_UINT:
        return 8;
    // 16 bpp
    case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT:
        return 16;
    default:
        return 0; // unknown — caller falls back
    }
}

// Helper: gather pixel data from mapped buffer memory for inline CopyBufToImg.
// Returns the contiguous data span [minOff, maxEnd) and adjusts bufOffsets to be relative to 0.
// bRL[i]: bufferRowLength (0 = tight), bIH[i]: bufferImageHeight (0 = tight), bpp: bytes/texel.
static std::vector<uint8_t> gatherCopySourceData(uint64_t bufId, uint32_t regionCount,
    uint32_t* bufOffsets, const uint32_t* extW, const uint32_t* extH, const uint32_t* extD,
    const uint32_t* bRL, const uint32_t* bIH, uint32_t bpp) {
    if (bpp == 0) return {};  // unknown format — can't calculate region size
    auto bit = g_icd.bufferBindings.find(bufId);
    if (bit == g_icd.bufferBindings.end()) return {};
    uint64_t memId = bit->second.memoryId;
    VkDeviceSize memBase = bit->second.memoryOffset;
    auto sit = g_icd.memoryShadows.find(memId);
    if (sit == g_icd.memoryShadows.end() || !sit->second.ptr) return {};

    // Find span [minOff, maxEnd) across all regions
    // rowPitch = (bufferRowLength != 0 ? bufferRowLength : imageExtent.width) * bpp
    // imgH     = (bufferImageHeight != 0 ? bufferImageHeight : imageExtent.height)
    uint32_t minOff = UINT32_MAX, maxEnd = 0;
    for (uint32_t i = 0; i < regionCount; i++) {
        uint32_t rowPitch = (bRL[i] != 0 ? bRL[i] : extW[i]) * bpp;
        uint32_t imgH     = (bIH[i] != 0 ? bIH[i] : extH[i]);
        uint32_t regSize  = rowPitch * imgH * extD[i];
        if (bufOffsets[i] < minOff) minOff = bufOffsets[i];
        uint32_t end = bufOffsets[i] + regSize;
        if (end > maxEnd) maxEnd = end;
    }
    if (minOff >= maxEnd) return {};
    uint32_t spanSize = maxEnd - minOff;

    // Copy from shadow memory
    const uint8_t* shadowBase = (const uint8_t*)sit->second.ptr;
    VkDeviceSize srcOff = memBase + minOff;
    if (srcOff + spanSize > sit->second.size) return {};
    std::vector<uint8_t> data(spanSize);
    memcpy(data.data(), shadowBase + srcOff, spanSize);

    // Rebase bufOffsets relative to 0
    for (uint32_t i = 0; i < regionCount; i++)
        bufOffsets[i] -= minOff;
    return data;
}

static void VKAPI_CALL icd_vkCmdCopyBufferToImage2(VkCommandBuffer cb, const void* pCopyInfo) {
    struct CopyBufToImgInfo2 {
        VkStructureType sType; const void* pNext;
        VkBuffer srcBuffer; VkImage dstImage; VkImageLayout dstImageLayout;
        uint32_t regionCount;
        const VkBufferImageCopy2* pRegions;
    };
    auto* info = static_cast<const CopyBufToImgInfo2*>(pCopyInfo);
    std::vector<uint32_t> bOff(info->regionCount), bRL(info->regionCount), bIH(info->regionCount);
    std::vector<uint32_t> iAsp(info->regionCount), iMip(info->regionCount), iBL(info->regionCount), iLC(info->regionCount);
    std::vector<int32_t> iOX(info->regionCount), iOY(info->regionCount), iOZ(info->regionCount);
    std::vector<uint32_t> iEW(info->regionCount), iEH(info->regionCount), iED(info->regionCount);
    for (uint32_t i = 0; i < info->regionCount; i++) {
        bOff[i] = (uint32_t)info->pRegions[i].bufferOffset; bRL[i] = info->pRegions[i].bufferRowLength; bIH[i] = info->pRegions[i].bufferImageHeight;
        iAsp[i] = info->pRegions[i].imageSubresource.aspectMask; iMip[i] = info->pRegions[i].imageSubresource.mipLevel;
        iBL[i] = info->pRegions[i].imageSubresource.baseArrayLayer; iLC[i] = info->pRegions[i].imageSubresource.layerCount;
        iOX[i] = info->pRegions[i].imageOffset.x; iOY[i] = info->pRegions[i].imageOffset.y; iOZ[i] = info->pRegions[i].imageOffset.z;
        iEW[i] = info->pRegions[i].imageExtent.width; iEH[i] = info->pRegions[i].imageExtent.height; iED[i] = info->pRegions[i].imageExtent.depth;
    }
    // Look up destination image format for correct bpp calculation
    uint32_t dstBpp;
    {
        auto fit = g_icd.imageFormats.find((uint64_t)info->dstImage);
        dstBpp = (fit != g_icd.imageFormats.end()) ? formatBpp(fit->second) : 4u;
    }
    // Inline data: gather pixel data and encode atomically with copy command
    auto pixelData = gatherCopySourceData((uint64_t)info->srcBuffer, info->regionCount,
        bOff.data(), iEW.data(), iEH.data(), iED.data(), bRL.data(), bIH.data(), dstBpp);
    if (!pixelData.empty()) {
        g_icd.encoder.cmdCopyBufferToImageInline(toId(cb), (uint64_t)info->dstImage,
            info->dstImageLayout, info->regionCount,
            bOff.data(), bRL.data(), bIH.data(), iAsp.data(), iMip.data(), iBL.data(), iLC.data(),
            iOX.data(), iOY.data(), iOZ.data(), iEW.data(), iEH.data(), iED.data(),
            (uint32_t)pixelData.size(), pixelData.data());
    } else {
        // Fallback: use original path with separate WriteMemory.
        // For unknown formats (dstBpp==0) default to 4 to preserve old behavior
        // (over-estimate is clipped by flushBufferRange; compressed formats still work).
        uint32_t fbBpp = (dstBpp != 0) ? dstBpp : 4u;
        for (uint32_t i = 0; i < info->regionCount; i++) {
            uint32_t rowPitch = (bRL[i] != 0 ? bRL[i] : iEW[i]) * fbBpp;
            uint32_t imgH     = (bIH[i] != 0 ? bIH[i] : iEH[i]);
            VkDeviceSize size = (VkDeviceSize)rowPitch * imgH * iED[i];
            g_icd.flushBufferRange((uint64_t)info->srcBuffer, info->pRegions[i].bufferOffset, size);
        }
        // Restore original offsets for non-inline path
        for (uint32_t i = 0; i < info->regionCount; i++)
            bOff[i] = (uint32_t)info->pRegions[i].bufferOffset;
        g_icd.encoder.cmdCopyBufferToImage(toId(cb), (uint64_t)info->srcBuffer, (uint64_t)info->dstImage,
            info->dstImageLayout, info->regionCount,
            bOff.data(), bRL.data(), bIH.data(), iAsp.data(), iMip.data(), iBL.data(), iLC.data(),
            iOX.data(), iOY.data(), iOZ.data(), iEW.data(), iEH.data(), iED.data());
    }
}
static void VKAPI_CALL icd_vkCmdCopyImage(VkCommandBuffer cb, VkImage srcImg, VkImageLayout srcLayout,
    VkImage dstImg, VkImageLayout dstLayout, uint32_t regionCount, const VkImageCopy* pRegions)
{
    ENC_LOCK;
    auto off = g_icd.encoder.w_.beginCommand(VN_CMD_vkCmdCopyImage);
    g_icd.encoder.w_.writeU64(toId(cb));
    g_icd.encoder.w_.writeU64((uint64_t)srcImg);
    g_icd.encoder.w_.writeU32((uint32_t)srcLayout);
    g_icd.encoder.w_.writeU64((uint64_t)dstImg);
    g_icd.encoder.w_.writeU32((uint32_t)dstLayout);
    g_icd.encoder.w_.writeU32(regionCount);
    for (uint32_t i = 0; i < regionCount; i++) {
        g_icd.encoder.w_.writeU32(pRegions[i].srcSubresource.aspectMask);
        g_icd.encoder.w_.writeU32(pRegions[i].srcSubresource.mipLevel);
        g_icd.encoder.w_.writeU32(pRegions[i].srcSubresource.baseArrayLayer);
        g_icd.encoder.w_.writeU32(pRegions[i].srcSubresource.layerCount);
        g_icd.encoder.w_.writeI32(pRegions[i].srcOffset.x);
        g_icd.encoder.w_.writeI32(pRegions[i].srcOffset.y);
        g_icd.encoder.w_.writeI32(pRegions[i].srcOffset.z);
        g_icd.encoder.w_.writeU32(pRegions[i].dstSubresource.aspectMask);
        g_icd.encoder.w_.writeU32(pRegions[i].dstSubresource.mipLevel);
        g_icd.encoder.w_.writeU32(pRegions[i].dstSubresource.baseArrayLayer);
        g_icd.encoder.w_.writeU32(pRegions[i].dstSubresource.layerCount);
        g_icd.encoder.w_.writeI32(pRegions[i].dstOffset.x);
        g_icd.encoder.w_.writeI32(pRegions[i].dstOffset.y);
        g_icd.encoder.w_.writeI32(pRegions[i].dstOffset.z);
        g_icd.encoder.w_.writeU32(pRegions[i].extent.width);
        g_icd.encoder.w_.writeU32(pRegions[i].extent.height);
        g_icd.encoder.w_.writeU32(pRegions[i].extent.depth);
    }
    g_icd.encoder.w_.endCommand(off);
}
static void VKAPI_CALL icd_vkCmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer buf, VkImage img, VkImageLayout layout,
    uint32_t regionCount, const VkBufferImageCopy* pRegions) {
    std::vector<uint32_t> bOff(regionCount), bRL(regionCount), bIH(regionCount);
    std::vector<uint32_t> iAsp(regionCount), iMip(regionCount), iBL(regionCount), iLC(regionCount);
    std::vector<int32_t> iOX(regionCount), iOY(regionCount), iOZ(regionCount);
    std::vector<uint32_t> iEW(regionCount), iEH(regionCount), iED(regionCount);
    for (uint32_t i = 0; i < regionCount; i++) {
        bOff[i] = (uint32_t)pRegions[i].bufferOffset; bRL[i] = pRegions[i].bufferRowLength; bIH[i] = pRegions[i].bufferImageHeight;
        iAsp[i] = pRegions[i].imageSubresource.aspectMask; iMip[i] = pRegions[i].imageSubresource.mipLevel;
        iBL[i] = pRegions[i].imageSubresource.baseArrayLayer; iLC[i] = pRegions[i].imageSubresource.layerCount;
        iOX[i] = pRegions[i].imageOffset.x; iOY[i] = pRegions[i].imageOffset.y; iOZ[i] = pRegions[i].imageOffset.z;
        iEW[i] = pRegions[i].imageExtent.width; iEH[i] = pRegions[i].imageExtent.height; iED[i] = pRegions[i].imageExtent.depth;
    }
    uint32_t dstBpp;
    {
        auto fit = g_icd.imageFormats.find((uint64_t)img);
        dstBpp = (fit != g_icd.imageFormats.end()) ? formatBpp(fit->second) : 4u;
    }
    auto pixelData = gatherCopySourceData((uint64_t)buf, regionCount,
        bOff.data(), iEW.data(), iEH.data(), iED.data(), bRL.data(), bIH.data(), dstBpp);
    if (!pixelData.empty()) {
        g_icd.encoder.cmdCopyBufferToImageInline(toId(cb), (uint64_t)img, layout, regionCount,
            bOff.data(), bRL.data(), bIH.data(), iAsp.data(), iMip.data(), iBL.data(), iLC.data(),
            iOX.data(), iOY.data(), iOZ.data(), iEW.data(), iEH.data(), iED.data(),
            (uint32_t)pixelData.size(), pixelData.data());
    } else {
        uint32_t fbBpp = (dstBpp != 0) ? dstBpp : 4u;
        for (uint32_t i = 0; i < regionCount; i++) {
            uint32_t rowPitch = (bRL[i] != 0 ? bRL[i] : iEW[i]) * fbBpp;
            uint32_t imgH     = (bIH[i] != 0 ? bIH[i] : iEH[i]);
            VkDeviceSize size = (VkDeviceSize)rowPitch * imgH * iED[i];
            g_icd.flushBufferRange((uint64_t)buf, pRegions[i].bufferOffset, size);
        }
        for (uint32_t i = 0; i < regionCount; i++)
            bOff[i] = (uint32_t)pRegions[i].bufferOffset;
        g_icd.encoder.cmdCopyBufferToImage(toId(cb), (uint64_t)buf, (uint64_t)img, layout, regionCount,
            bOff.data(), bRL.data(), bIH.data(), iAsp.data(), iMip.data(), iBL.data(), iLC.data(),
            iOX.data(), iOY.data(), iOZ.data(), iEW.data(), iEH.data(), iED.data());
    }
}
// vkCmdCopyImage2 / vkCmdCopyImage2KHR (Vulkan 1.3)
static void VKAPI_CALL icd_vkCmdCopyImage2(VkCommandBuffer cb, const VkCopyImageInfo2* pInfo) {
    // Convert VkCopyImageInfo2 → encode as CmdCopyImage
    ENC_LOCK;
    auto off = g_icd.encoder.w_.beginCommand(VN_CMD_vkCmdCopyImage);
    g_icd.encoder.w_.writeU64(toId(cb));
    g_icd.encoder.w_.writeU64((uint64_t)pInfo->srcImage);
    g_icd.encoder.w_.writeU32((uint32_t)pInfo->srcImageLayout);
    g_icd.encoder.w_.writeU64((uint64_t)pInfo->dstImage);
    g_icd.encoder.w_.writeU32((uint32_t)pInfo->dstImageLayout);
    g_icd.encoder.w_.writeU32(pInfo->regionCount);
    for (uint32_t i = 0; i < pInfo->regionCount; i++) {
        const auto& r = pInfo->pRegions[i];
        g_icd.encoder.w_.writeU32(r.srcSubresource.aspectMask);
        g_icd.encoder.w_.writeU32(r.srcSubresource.mipLevel);
        g_icd.encoder.w_.writeU32(r.srcSubresource.baseArrayLayer);
        g_icd.encoder.w_.writeU32(r.srcSubresource.layerCount);
        g_icd.encoder.w_.writeI32(r.srcOffset.x);
        g_icd.encoder.w_.writeI32(r.srcOffset.y);
        g_icd.encoder.w_.writeI32(r.srcOffset.z);
        g_icd.encoder.w_.writeU32(r.dstSubresource.aspectMask);
        g_icd.encoder.w_.writeU32(r.dstSubresource.mipLevel);
        g_icd.encoder.w_.writeU32(r.dstSubresource.baseArrayLayer);
        g_icd.encoder.w_.writeU32(r.dstSubresource.layerCount);
        g_icd.encoder.w_.writeI32(r.dstOffset.x);
        g_icd.encoder.w_.writeI32(r.dstOffset.y);
        g_icd.encoder.w_.writeI32(r.dstOffset.z);
        g_icd.encoder.w_.writeU32(r.extent.width);
        g_icd.encoder.w_.writeU32(r.extent.height);
        g_icd.encoder.w_.writeU32(r.extent.depth);
    }
    g_icd.encoder.w_.endCommand(off);
}

// vkCmdBlitImage (core 1.0)
static void VKAPI_CALL icd_vkCmdBlitImage(VkCommandBuffer cb, VkImage srcImg, VkImageLayout srcLayout,
    VkImage dstImg, VkImageLayout dstLayout, uint32_t regionCount, const VkImageBlit* pRegions, VkFilter filter)
{
    ENC_LOCK;
    auto off = g_icd.encoder.w_.beginCommand(VN_CMD_vkCmdBlitImage);
    g_icd.encoder.w_.writeU64(toId(cb));
    g_icd.encoder.w_.writeU64((uint64_t)srcImg);
    g_icd.encoder.w_.writeU32((uint32_t)srcLayout);
    g_icd.encoder.w_.writeU64((uint64_t)dstImg);
    g_icd.encoder.w_.writeU32((uint32_t)dstLayout);
    g_icd.encoder.w_.writeU32(regionCount);
    for (uint32_t i = 0; i < regionCount; i++) {
        const auto& r = pRegions[i];
        g_icd.encoder.w_.writeU32(r.srcSubresource.aspectMask);
        g_icd.encoder.w_.writeU32(r.srcSubresource.mipLevel);
        g_icd.encoder.w_.writeU32(r.srcSubresource.baseArrayLayer);
        g_icd.encoder.w_.writeU32(r.srcSubresource.layerCount);
        for (int j = 0; j < 2; j++) {
            g_icd.encoder.w_.writeI32(r.srcOffsets[j].x);
            g_icd.encoder.w_.writeI32(r.srcOffsets[j].y);
            g_icd.encoder.w_.writeI32(r.srcOffsets[j].z);
        }
        g_icd.encoder.w_.writeU32(r.dstSubresource.aspectMask);
        g_icd.encoder.w_.writeU32(r.dstSubresource.mipLevel);
        g_icd.encoder.w_.writeU32(r.dstSubresource.baseArrayLayer);
        g_icd.encoder.w_.writeU32(r.dstSubresource.layerCount);
        for (int j = 0; j < 2; j++) {
            g_icd.encoder.w_.writeI32(r.dstOffsets[j].x);
            g_icd.encoder.w_.writeI32(r.dstOffsets[j].y);
            g_icd.encoder.w_.writeI32(r.dstOffsets[j].z);
        }
    }
    g_icd.encoder.w_.writeU32((uint32_t)filter);
    g_icd.encoder.w_.endCommand(off);
}

// vkCmdBlitImage2 / vkCmdBlitImage2KHR (Vulkan 1.3)
static void VKAPI_CALL icd_vkCmdBlitImage2(VkCommandBuffer cb, const VkBlitImageInfo2* pInfo) {
    ENC_LOCK;
    auto off = g_icd.encoder.w_.beginCommand(VN_CMD_vkCmdBlitImage);
    g_icd.encoder.w_.writeU64(toId(cb));
    g_icd.encoder.w_.writeU64((uint64_t)pInfo->srcImage);
    g_icd.encoder.w_.writeU32((uint32_t)pInfo->srcImageLayout);
    g_icd.encoder.w_.writeU64((uint64_t)pInfo->dstImage);
    g_icd.encoder.w_.writeU32((uint32_t)pInfo->dstImageLayout);
    g_icd.encoder.w_.writeU32(pInfo->regionCount);
    for (uint32_t i = 0; i < pInfo->regionCount; i++) {
        const auto& r = pInfo->pRegions[i];
        g_icd.encoder.w_.writeU32(r.srcSubresource.aspectMask);
        g_icd.encoder.w_.writeU32(r.srcSubresource.mipLevel);
        g_icd.encoder.w_.writeU32(r.srcSubresource.baseArrayLayer);
        g_icd.encoder.w_.writeU32(r.srcSubresource.layerCount);
        for (int j = 0; j < 2; j++) {
            g_icd.encoder.w_.writeI32(r.srcOffsets[j].x);
            g_icd.encoder.w_.writeI32(r.srcOffsets[j].y);
            g_icd.encoder.w_.writeI32(r.srcOffsets[j].z);
        }
        g_icd.encoder.w_.writeU32(r.dstSubresource.aspectMask);
        g_icd.encoder.w_.writeU32(r.dstSubresource.mipLevel);
        g_icd.encoder.w_.writeU32(r.dstSubresource.baseArrayLayer);
        g_icd.encoder.w_.writeU32(r.dstSubresource.layerCount);
        for (int j = 0; j < 2; j++) {
            g_icd.encoder.w_.writeI32(r.dstOffsets[j].x);
            g_icd.encoder.w_.writeI32(r.dstOffsets[j].y);
            g_icd.encoder.w_.writeI32(r.dstOffsets[j].z);
        }
    }
    g_icd.encoder.w_.writeU32((uint32_t)pInfo->filter);
    g_icd.encoder.w_.endCommand(off);
}

static void VKAPI_CALL icd_vkCmdCopyImageToBuffer(VkCommandBuffer, VkImage, VkImageLayout, VkBuffer, uint32_t, const VkBufferImageCopy*) {}
static void VKAPI_CALL icd_vkCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
    VkDependencyFlags, uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*,
    uint32_t imageBarrierCount, const VkImageMemoryBarrier* pImageBarriers) {
    if (imageBarrierCount == 0) return;
    std::vector<uint64_t> images(imageBarrierCount);
    std::vector<uint32_t> oldLayouts(imageBarrierCount), newLayouts(imageBarrierCount);
    std::vector<uint32_t> srcAccess(imageBarrierCount), dstAccess(imageBarrierCount);
    for (uint32_t i = 0; i < imageBarrierCount; i++) {
        images[i] = (uint64_t)pImageBarriers[i].image;
        oldLayouts[i] = pImageBarriers[i].oldLayout;
        newLayouts[i] = pImageBarriers[i].newLayout;
        srcAccess[i] = pImageBarriers[i].srcAccessMask;
        dstAccess[i] = pImageBarriers[i].dstAccessMask;
    }
    g_icd.encoder.cmdPipelineBarrier(toId(cb), srcStage, dstStage,
        imageBarrierCount, images.data(), oldLayouts.data(), newLayouts.data(),
        srcAccess.data(), dstAccess.data());
}
static void VKAPI_CALL icd_vkCmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo* pInfo) {
    if (!pInfo || pInfo->imageMemoryBarrierCount == 0) return;
    std::vector<uint64_t> images(pInfo->imageMemoryBarrierCount);
    std::vector<uint32_t> oldLayouts(pInfo->imageMemoryBarrierCount), newLayouts(pInfo->imageMemoryBarrierCount);
    std::vector<uint32_t> srcAccess(pInfo->imageMemoryBarrierCount), dstAccess(pInfo->imageMemoryBarrierCount);
    // VkImageMemoryBarrier2 uses VkPipelineStageFlags2 — take from first barrier for simplicity
    uint32_t srcStage = 0, dstStage = 0;
    for (uint32_t i = 0; i < pInfo->imageMemoryBarrierCount; i++) {
        const auto& b = pInfo->pImageMemoryBarriers[i];
        images[i] = (uint64_t)b.image;
        oldLayouts[i] = b.oldLayout;
        newLayouts[i] = b.newLayout;
        srcAccess[i] = (uint32_t)b.srcAccessMask;
        dstAccess[i] = (uint32_t)b.dstAccessMask;
        srcStage |= (uint32_t)b.srcStageMask;
        dstStage |= (uint32_t)b.dstStageMask;
    }
    g_icd.encoder.cmdPipelineBarrier(toId(cb), srcStage, dstStage,
        pInfo->imageMemoryBarrierCount, images.data(), oldLayouts.data(), newLayouts.data(),
        srcAccess.data(), dstAccess.data());
}
static void VKAPI_CALL icd_vkCmdClearColorImage(VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue*, uint32_t, const VkImageSubresourceRange*) {
    // No-op: handled by forcing LOAD_OP_CLEAR in host BeginRendering
}
static void VKAPI_CALL icd_vkCmdClearAttachments(VkCommandBuffer cb, uint32_t attachmentCount, const VkClearAttachment* pAttachments, uint32_t rectCount, const VkClearRect* pRects) {
    g_icd.encoder.cmdClearAttachments(toId(cb), attachmentCount, pAttachments, rectCount, pRects);
}
static void VKAPI_CALL icd_vkCmdSetStencilReference(VkCommandBuffer, VkStencilFaceFlags, uint32_t) {}
static void VKAPI_CALL icd_vkCmdSetBlendConstants(VkCommandBuffer, const float[4]) {}
static void VKAPI_CALL icd_vkCmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout,
    VkShaderStageFlags stageFlags, uint32_t offset, uint32_t size, const void* pValues) {
    g_icd.encoder.cmdPushConstants(toId(cb), (uint64_t)layout, stageFlags, offset, size, pValues);
}
static void VKAPI_CALL icd_vkCmdDispatch(VkCommandBuffer, uint32_t, uint32_t, uint32_t) {}
static void VKAPI_CALL icd_vkCmdFillBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, uint32_t) {}
static void VKAPI_CALL icd_vkCmdUpdateBuffer(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize offset, VkDeviceSize dataSize, const void* pData) {
    g_icd.encoder.cmdUpdateBuffer(toId(cb), (uint64_t)buf, offset, dataSize, pData);
}
static void VKAPI_CALL icd_vkCmdResolveImage(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageResolve*) {}
static void VKAPI_CALL icd_vkCmdSetEvent(VkCommandBuffer, VkEvent, VkPipelineStageFlags) {}
static void VKAPI_CALL icd_vkCmdResetEvent(VkCommandBuffer, VkEvent, VkPipelineStageFlags) {}
static void VKAPI_CALL icd_vkCmdSetEvent2(VkCommandBuffer, VkEvent, const VkDependencyInfo*) {}
static void VKAPI_CALL icd_vkCmdResetEvent2(VkCommandBuffer, VkEvent, VkPipelineStageFlags2) {}
static void VKAPI_CALL icd_vkCmdWriteTimestamp(VkCommandBuffer, VkPipelineStageFlagBits, VkQueryPool, uint32_t) {}
static void VKAPI_CALL icd_vkCmdWriteTimestamp2(VkCommandBuffer, VkPipelineStageFlags2, VkQueryPool, uint32_t) {}
static void VKAPI_CALL icd_vkCmdBeginQuery(VkCommandBuffer, VkQueryPool, uint32_t, VkQueryControlFlags) {}
static void VKAPI_CALL icd_vkCmdEndQuery(VkCommandBuffer, VkQueryPool, uint32_t) {}
static void VKAPI_CALL icd_vkCmdResetQueryPool(VkCommandBuffer, VkQueryPool, uint32_t, uint32_t) {}
static void VKAPI_CALL icd_vkCmdCopyQueryPoolResults(VkCommandBuffer, VkQueryPool, uint32_t, uint32_t, VkBuffer, VkDeviceSize, VkDeviceSize, VkQueryResultFlags) {}

// --- Event / Query / misc ---

static VkResult VKAPI_CALL icd_vkCreateEvent(VkDevice, const VkEventCreateInfo*, const VkAllocationCallbacks*, VkEvent* p) {
    *p = (VkEvent)g_icd.handles.alloc(); return VK_SUCCESS;
}
static void VKAPI_CALL icd_vkDestroyEvent(VkDevice, VkEvent, const VkAllocationCallbacks*) {}
static VkResult VKAPI_CALL icd_vkCreateQueryPool(VkDevice, const VkQueryPoolCreateInfo*, const VkAllocationCallbacks*, VkQueryPool* p) {
    *p = (VkQueryPool)g_icd.handles.alloc(); return VK_SUCCESS;
}
static void VKAPI_CALL icd_vkDestroyQueryPool(VkDevice, VkQueryPool, const VkAllocationCallbacks*) {}
static VkResult VKAPI_CALL icd_vkGetQueryPoolResults(VkDevice, VkQueryPool, uint32_t, uint32_t, size_t, void*, VkDeviceSize, VkQueryResultFlags) {
    return VK_NOT_READY;
}
static void VKAPI_CALL icd_vkResetQueryPool(VkDevice, VkQueryPool, uint32_t, uint32_t) {}

static VkResult VKAPI_CALL icd_vkCreateComputePipelines(VkDevice, VkPipelineCache, uint32_t count, const VkComputePipelineCreateInfo*,
    const VkAllocationCallbacks*, VkPipeline* p) {
    for (uint32_t i = 0; i < count; i++) p[i] = (VkPipeline)g_icd.handles.alloc();
    return VK_SUCCESS;
}

static void VKAPI_CALL icd_vkGetPhysicalDeviceSparseImageFormatProperties(VkPhysicalDevice, VkFormat, VkImageType,
    VkSampleCountFlagBits, VkImageUsageFlags, VkImageTiling, uint32_t* pCount, VkSparseImageFormatProperties*) { *pCount = 0; }

static VkResult VKAPI_CALL icd_vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice, VkFormat, VkImageType,
    VkImageTiling, VkImageUsageFlags, VkImageCreateFlags, VkImageFormatProperties* p) {
    p->maxExtent = { 16384, 16384, 2048 };
    p->maxMipLevels = 15;
    p->maxArrayLayers = 2048;
    p->sampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    p->maxResourceSize = 2ull * 1024 * 1024 * 1024;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL icd_vkGetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice pd,
    const VkPhysicalDeviceImageFormatInfo2* pInfo, VkImageFormatProperties2* p) {
    return icd_vkGetPhysicalDeviceImageFormatProperties(pd, pInfo->format, pInfo->type,
        pInfo->tiling, pInfo->usage, pInfo->flags, &p->imageFormatProperties);
}

static void VKAPI_CALL icd_vkFlushMappedMemoryRanges() {}
static void VKAPI_CALL icd_vkInvalidateMappedMemoryRanges() {}

// --- Additional stubs DXVK queries ---

static void VKAPI_CALL icd_vkGetPhysicalDeviceExternalSemaphoreProperties(VkPhysicalDevice, const VkPhysicalDeviceExternalSemaphoreInfo*, VkExternalSemaphoreProperties* p) {
    memset(p, 0, sizeof(*p));
    p->sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES;
}

static void VKAPI_CALL icd_vkGetPhysicalDeviceSparseImageFormatProperties2(VkPhysicalDevice, const VkPhysicalDeviceSparseImageFormatInfo2*, uint32_t* pCount, VkSparseImageFormatProperties2*) { *pCount = 0; }

static VkResult VKAPI_CALL icd_vkGetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice pd, const VkPhysicalDeviceSurfaceInfo2KHR*, VkSurfaceCapabilities2KHR* p) {
    return icd_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, VK_NULL_HANDLE, &p->surfaceCapabilities);
}

static VkResult VKAPI_CALL icd_vkGetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR*, uint32_t* pCount, VkSurfaceFormat2KHR* p) {
    if (!p) { *pCount = (uint32_t)g_icd.surfaceFormats.size(); return VK_SUCCESS; }
    uint32_t n = (uint32_t)g_icd.surfaceFormats.size();
    if (*pCount < n) n = *pCount;
    for (uint32_t i = 0; i < n; i++) {
        p[i].sType = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR;
        p[i].surfaceFormat = g_icd.surfaceFormats[i];
    }
    *pCount = n;
    return VK_SUCCESS;
}

static VkBool32 VKAPI_CALL icd_vkGetPhysicalDeviceWin32PresentationSupportKHR(VkPhysicalDevice, uint32_t) { return VK_TRUE; }

// Debug utils - no-ops
static void VKAPI_CALL icd_vkCmdBeginDebugUtilsLabelEXT(VkCommandBuffer, const VkDebugUtilsLabelEXT*) {}
static void VKAPI_CALL icd_vkCmdEndDebugUtilsLabelEXT(VkCommandBuffer) {}
static void VKAPI_CALL icd_vkCmdInsertDebugUtilsLabelEXT(VkCommandBuffer, const VkDebugUtilsLabelEXT*) {}
static VkResult VKAPI_CALL icd_vkCreateDebugUtilsMessengerEXT(VkInstance, const VkDebugUtilsMessengerCreateInfoEXT*, const VkAllocationCallbacks*, VkDebugUtilsMessengerEXT* p) {
    *p = (VkDebugUtilsMessengerEXT)g_icd.handles.alloc(); return VK_SUCCESS;
}
static void VKAPI_CALL icd_vkDestroyDebugUtilsMessengerEXT(VkInstance, VkDebugUtilsMessengerEXT, const VkAllocationCallbacks*) {}
static void VKAPI_CALL icd_vkSubmitDebugUtilsMessageEXT(VkInstance, VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT*) {}

static VkResult VKAPI_CALL icd_vkGetPhysicalDeviceSurfacePresentModes2EXT(VkPhysicalDevice pd, const VkPhysicalDeviceSurfaceInfo2KHR*, uint32_t* pCount, VkPresentModeKHR* p) {
    return icd_vkGetPhysicalDeviceSurfacePresentModesKHR(pd, VK_NULL_HANDLE, pCount, p);
}

static VkResult VKAPI_CALL icd_vkReleaseSwapchainImagesEXT(VkDevice, const void*) { return VK_SUCCESS; }

// --- GetDeviceProcAddr ---

static PFN_vkVoidFunction VKAPI_CALL icd_vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    return lookupFunc(pName);
}

// =============================================================
// Function dispatch table
// =============================================================

struct FuncEntry {
    const char* name;
    PFN_vkVoidFunction func;
};

#define ENTRY(fn) { #fn, reinterpret_cast<PFN_vkVoidFunction>(icd_##fn) }

// --- Proper mocks for display/debug query functions ---
// Return well-defined "not available" results instead of leaving output uninitialized.
static void VKAPI_CALL icd_vkDebugReportMessageEXT(VkInstance, VkDebugReportFlagsEXT, VkDebugReportObjectTypeEXT,
    uint64_t, size_t, int32_t, const char*, const char*) {}
static void VKAPI_CALL icd_vkDestroyDebugReportCallbackEXT(VkInstance, VkDebugReportCallbackEXT, const VkAllocationCallbacks*) {}
static VkResult VKAPI_CALL icd_vkCreateDebugReportCallbackEXT(VkInstance, const void*, const VkAllocationCallbacks*, VkDebugReportCallbackEXT* p) {
    *p = (VkDebugReportCallbackEXT)1; return VK_SUCCESS;
}
static VkResult VKAPI_CALL icd_vkGetDisplayPlaneSupportedDisplaysKHR(VkPhysicalDevice, uint32_t, uint32_t* pCount, VkDisplayKHR*) {
    *pCount = 0; return VK_SUCCESS;
}
static VkResult VKAPI_CALL icd_vkGetDisplayModePropertiesKHR(VkPhysicalDevice, VkDisplayKHR, uint32_t* pCount, void*) {
    *pCount = 0; return VK_SUCCESS;
}
static VkResult VKAPI_CALL icd_vkGetDisplayPlaneCapabilitiesKHR(VkPhysicalDevice, VkDisplayModeKHR, uint32_t, VkDisplayPlaneCapabilitiesKHR* p) {
    memset(p, 0, sizeof(*p)); return VK_SUCCESS;
}
static VkResult VKAPI_CALL icd_vkGetDisplayModeProperties2KHR(VkPhysicalDevice, VkDisplayKHR, uint32_t* pCount, void*) {
    *pCount = 0; return VK_SUCCESS;
}
static VkResult VKAPI_CALL icd_vkGetDisplayPlaneCapabilities2KHR(VkPhysicalDevice, const void*, VkDisplayPlaneCapabilities2KHR* p) {
    memset(p, 0, sizeof(*p)); p->sType = VK_STRUCTURE_TYPE_DISPLAY_PLANE_CAPABILITIES_2_KHR; return VK_SUCCESS;
}
static VkResult VKAPI_CALL icd_vkReleaseDisplayEXT(VkPhysicalDevice, VkDisplayKHR) { return VK_SUCCESS; }
static VkResult VKAPI_CALL icd_vkGetPhysicalDeviceSurfaceCapabilities2EXT(VkPhysicalDevice, VkSurfaceKHR, void* pCaps) {
    memset(pCaps, 0, sizeof(VkSurfaceCapabilities2EXT));
    auto* c = static_cast<VkSurfaceCapabilities2EXT*>(pCaps);
    c->sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_EXT;
    c->minImageCount = 2; c->maxImageCount = 3;
    c->currentExtent = {800, 600}; c->minImageExtent = {1,1}; c->maxImageExtent = {4096,4096};
    c->maxImageArrayLayers = 1; c->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    c->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    c->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    c->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL icd_vkGetDrmDisplayEXT(VkPhysicalDevice, int32_t, uint32_t, VkDisplayKHR* p) {
    *p = VK_NULL_HANDLE; return VK_ERROR_INITIALIZATION_FAILED;
}
static VkResult VKAPI_CALL icd_vkGetWinrtDisplayNV(VkPhysicalDevice, uint32_t, VkDisplayKHR* p) {
    *p = VK_NULL_HANDLE; return VK_ERROR_INITIALIZATION_FAILED;
}
static void VKAPI_CALL icd_vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR(
    VkPhysicalDevice, const void*, uint32_t* pNumPasses) { *pNumPasses = 0; }

static const FuncEntry g_funcTable[] = {
    // Instance
    ENTRY(vkCreateInstance),
    ENTRY(vkDestroyInstance),
    ENTRY(vkEnumeratePhysicalDevices),
    ENTRY(vkEnumerateInstanceExtensionProperties),
    ENTRY(vkEnumerateInstanceLayerProperties),
    ENTRY(vkEnumerateInstanceVersion),
    ENTRY(vkGetDeviceProcAddr),

    // Physical device
    ENTRY(vkGetPhysicalDeviceProperties),
    ENTRY(vkGetPhysicalDeviceProperties2),
    ENTRY(vkGetPhysicalDeviceFeatures),
    ENTRY(vkGetPhysicalDeviceFeatures2),
    ENTRY(vkGetPhysicalDeviceMemoryProperties),
    ENTRY(vkGetPhysicalDeviceMemoryProperties2),
    ENTRY(vkGetPhysicalDeviceQueueFamilyProperties),
    ENTRY(vkGetPhysicalDeviceQueueFamilyProperties2),
    ENTRY(vkEnumerateDeviceExtensionProperties),
    ENTRY(vkGetPhysicalDeviceFormatProperties),
    ENTRY(vkGetPhysicalDeviceFormatProperties2),
    ENTRY(vkGetPhysicalDeviceImageFormatProperties),
    ENTRY(vkGetPhysicalDeviceImageFormatProperties2),
    ENTRY(vkGetPhysicalDeviceSparseImageFormatProperties),

    // Surface
    ENTRY(vkCreateWin32SurfaceKHR),
    ENTRY(vkDestroySurfaceKHR),
    ENTRY(vkGetPhysicalDeviceSurfaceSupportKHR),
    ENTRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR),
    ENTRY(vkGetPhysicalDeviceSurfaceFormatsKHR),
    ENTRY(vkGetPhysicalDeviceSurfacePresentModesKHR),

    // Device
    ENTRY(vkCreateDevice),
    ENTRY(vkDestroyDevice),
    ENTRY(vkGetDeviceQueue),
    ENTRY(vkDeviceWaitIdle),
    ENTRY(vkQueueWaitIdle),
    ENTRY(vkGetDeviceQueue2),
    ENTRY(vkBindBufferMemory2),
    ENTRY(vkBindImageMemory2),
    ENTRY(vkGetBufferDeviceAddress),
    ENTRY(vkGetBufferOpaqueCaptureAddress),
    ENTRY(vkGetDeviceMemoryOpaqueCaptureAddress),
    ENTRY(vkGetSemaphoreCounterValue),
    ENTRY(vkWaitSemaphores),
    ENTRY(vkSignalSemaphore),

    // Swapchain
    ENTRY(vkCreateSwapchainKHR),
    ENTRY(vkDestroySwapchainKHR),
    ENTRY(vkGetSwapchainImagesKHR),
    ENTRY(vkAcquireNextImageKHR),
    ENTRY(vkQueuePresentKHR),

    // Memory
    ENTRY(vkAllocateMemory),
    ENTRY(vkFreeMemory),
    ENTRY(vkMapMemory),
    ENTRY(vkMapMemory2),
    {"vkMapMemory2KHR", (PFN_vkVoidFunction)icd_vkMapMemory2},
    ENTRY(vkUnmapMemory2),
    {"vkUnmapMemory2KHR", (PFN_vkVoidFunction)icd_vkUnmapMemory2},
    ENTRY(vkUnmapMemory),
    ENTRY(vkBindBufferMemory),
    ENTRY(vkBindImageMemory),
    ENTRY(vkGetBufferMemoryRequirements),
    ENTRY(vkGetImageMemoryRequirements),
    ENTRY(vkGetBufferMemoryRequirements2),
    ENTRY(vkGetImageMemoryRequirements2),
    ENTRY(vkGetDeviceBufferMemoryRequirements),
    ENTRY(vkGetDeviceImageMemoryRequirements),
    ENTRY(vkFlushMappedMemoryRanges),
    ENTRY(vkInvalidateMappedMemoryRanges),

    // Buffer / Image
    ENTRY(vkCreateBuffer),
    ENTRY(vkDestroyBuffer),
    ENTRY(vkCreateImage),
    ENTRY(vkDestroyImage),
    ENTRY(vkCreateImageView),
    {"vkGetImageViewHandleNVX", (PFN_vkVoidFunction)icd_vkGetImageViewHandleNVX},
    {"vkGetImageViewHandle64NVX", (PFN_vkVoidFunction)icd_vkGetImageViewHandleNVX},
    {"vkGetImageViewAddressNVX", (PFN_vkVoidFunction)icd_vkGetImageViewAddressNVX},
    ENTRY(vkDestroyImageView),

    // Shader / Pipeline
    ENTRY(vkCreateShaderModule),
    ENTRY(vkDestroyShaderModule),
    ENTRY(vkCreatePipelineLayout),
    ENTRY(vkDestroyPipelineLayout),
    ENTRY(vkCreateGraphicsPipelines),
    ENTRY(vkCreateComputePipelines),
    ENTRY(vkDestroyPipeline),
    ENTRY(vkCreatePipelineCache),
    ENTRY(vkDestroyPipelineCache),
    ENTRY(vkCreateRenderPass),
    ENTRY(vkCreateRenderPass2),
    ENTRY(vkDestroyRenderPass),
    ENTRY(vkCreateFramebuffer),
    ENTRY(vkDestroyFramebuffer),

    // Sampler / Descriptor
    ENTRY(vkCreateSampler),
    ENTRY(vkDestroySampler),
    ENTRY(vkCreateDescriptorSetLayout),
    ENTRY(vkDestroyDescriptorSetLayout),
    ENTRY(vkCreateDescriptorPool),
    ENTRY(vkDestroyDescriptorPool),
    ENTRY(vkResetDescriptorPool),
    ENTRY(vkAllocateDescriptorSets),
    ENTRY(vkFreeDescriptorSets),
    ENTRY(vkUpdateDescriptorSets),

    // Command pool / buffer
    ENTRY(vkCreateCommandPool),
    ENTRY(vkDestroyCommandPool),
    ENTRY(vkResetCommandPool),
    ENTRY(vkAllocateCommandBuffers),
    ENTRY(vkFreeCommandBuffers),
    ENTRY(vkBeginCommandBuffer),
    ENTRY(vkEndCommandBuffer),
    ENTRY(vkResetCommandBuffer),

    // Command recording
    ENTRY(vkCmdBeginRenderPass),
    ENTRY(vkCmdBeginRenderPass2),
    ENTRY(vkCmdEndRenderPass),
    ENTRY(vkCmdEndRenderPass2),
    ENTRY(vkCmdNextSubpass),
    ENTRY(vkCmdNextSubpass2),
    ENTRY(vkCmdBindPipeline),
    ENTRY(vkCmdSetViewport),
    ENTRY(vkCmdSetScissor),
    ENTRY(vkCmdDraw),
    ENTRY(vkCmdDrawIndexed),
    ENTRY(vkCmdBindDescriptorSets),
    ENTRY(vkCmdBindVertexBuffers),
    ENTRY(vkCmdBindIndexBuffer),
    ENTRY(vkCmdBindIndexBuffer2),
    {"vkCmdBindIndexBuffer2KHR", (PFN_vkVoidFunction)icd_vkCmdBindIndexBuffer2},

    // Proper mocks for display/debug query functions
    ENTRY(vkDebugReportMessageEXT),
    ENTRY(vkDestroyDebugReportCallbackEXT),
    ENTRY(vkCreateDebugReportCallbackEXT),
    ENTRY(vkGetDisplayPlaneSupportedDisplaysKHR),
    ENTRY(vkGetDisplayModePropertiesKHR),
    ENTRY(vkGetDisplayPlaneCapabilitiesKHR),
    ENTRY(vkGetDisplayModeProperties2KHR),
    ENTRY(vkGetDisplayPlaneCapabilities2KHR),
    ENTRY(vkReleaseDisplayEXT),
    ENTRY(vkGetPhysicalDeviceSurfaceCapabilities2EXT),
    ENTRY(vkGetDrmDisplayEXT),
    ENTRY(vkGetWinrtDisplayNV),
    ENTRY(vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR),
    ENTRY(vkCmdCopyBuffer),
    {"vkCmdCopyBuffer2", (PFN_vkVoidFunction)icd_vkCmdCopyBuffer2},
    {"vkCmdCopyBuffer2KHR", (PFN_vkVoidFunction)icd_vkCmdCopyBuffer2},
    ENTRY(vkCmdCopyImage),
    ENTRY(vkCmdCopyImage2),
    {"vkCmdCopyImage2KHR", (PFN_vkVoidFunction)icd_vkCmdCopyImage2},
    ENTRY(vkCmdBlitImage),
    ENTRY(vkCmdBlitImage2),
    {"vkCmdBlitImage2KHR", (PFN_vkVoidFunction)icd_vkCmdBlitImage2},
    ENTRY(vkCmdCopyBufferToImage),
    {"vkCmdCopyBufferToImage2", (PFN_vkVoidFunction)icd_vkCmdCopyBufferToImage2},
    {"vkCmdCopyBufferToImage2KHR", (PFN_vkVoidFunction)icd_vkCmdCopyBufferToImage2},
    ENTRY(vkCmdCopyImageToBuffer),
    ENTRY(vkCmdPipelineBarrier),
    ENTRY(vkCmdPipelineBarrier2),
    ENTRY(vkCmdClearColorImage),
    ENTRY(vkCmdClearAttachments),
    ENTRY(vkCmdSetStencilReference),
    ENTRY(vkCmdSetBlendConstants),
    ENTRY(vkCmdPushConstants),
    ENTRY(vkCmdDispatch),
    ENTRY(vkCmdFillBuffer),
    ENTRY(vkCmdUpdateBuffer),
    ENTRY(vkCmdResolveImage),
    ENTRY(vkCmdSetEvent),
    ENTRY(vkCmdResetEvent),
    ENTRY(vkCmdSetEvent2),
    ENTRY(vkCmdResetEvent2),
    ENTRY(vkCmdWriteTimestamp),
    ENTRY(vkCmdWriteTimestamp2),
    ENTRY(vkCmdBeginQuery),
    ENTRY(vkCmdEndQuery),
    ENTRY(vkCmdResetQueryPool),
    ENTRY(vkCmdCopyQueryPoolResults),

    // Vulkan 1.3 dynamic rendering / dynamic state
    ENTRY(vkCmdBeginRendering),
    ENTRY(vkCmdEndRendering),
    ENTRY(vkCmdSetCullMode),
    ENTRY(vkCmdSetFrontFace),
    ENTRY(vkCmdSetPrimitiveTopology),
    ENTRY(vkCmdSetViewportWithCount),
    ENTRY(vkCmdSetScissorWithCount),
    ENTRY(vkCmdSetDepthTestEnable),
    ENTRY(vkCmdSetDepthWriteEnable),
    ENTRY(vkCmdSetDepthCompareOp),
    ENTRY(vkCmdSetDepthBoundsTestEnable),
    ENTRY(vkCmdSetStencilTestEnable),
    ENTRY(vkCmdSetStencilOp),
    ENTRY(vkCmdSetRasterizerDiscardEnable),
    ENTRY(vkCmdSetDepthBiasEnable),
    ENTRY(vkCmdSetPrimitiveRestartEnable),
    ENTRY(vkCmdBindVertexBuffers2),
    ENTRY(vkCmdSetDepthBounds),
    ENTRY(vkCmdDrawIndirect),
    ENTRY(vkCmdDrawIndexedIndirect),

    // Descriptor update template
    ENTRY(vkCreateDescriptorUpdateTemplate),
    ENTRY(vkDestroyDescriptorUpdateTemplate),
    ENTRY(vkUpdateDescriptorSetWithTemplate),
    // Bind descriptor sets v2 (Vulkan 1.4 / VK_KHR_maintenance6)
    {"vkCmdBindDescriptorSets2KHR", (PFN_vkVoidFunction)icd_vkCmdBindDescriptorSets2KHR},
    {"vkCmdBindDescriptorSets2", (PFN_vkVoidFunction)icd_vkCmdBindDescriptorSets2KHR},
    // Push descriptors — DXVK uses this instead of alloc+update+bind
    {"vkCmdPushDescriptorSetKHR", (PFN_vkVoidFunction)icd_vkCmdPushDescriptorSetKHR},
    {"vkCmdPushDescriptorSet", (PFN_vkVoidFunction)icd_vkCmdPushDescriptorSetKHR},
    {"vkCmdPushDescriptorSet2KHR", (PFN_vkVoidFunction)icd_vkCmdPushDescriptorSetKHR},
    {"vkCmdPushDescriptorSet2", (PFN_vkVoidFunction)icd_vkCmdPushDescriptorSetKHR},
    {"vkCmdPushDescriptorSetWithTemplateKHR", (PFN_vkVoidFunction)icd_vkCmdPushDescriptorSetWithTemplateKHR},
    {"vkCmdPushDescriptorSetWithTemplate", (PFN_vkVoidFunction)icd_vkCmdPushDescriptorSetWithTemplateKHR},
    {"vkCmdPushDescriptorSetWithTemplate2KHR", (PFN_vkVoidFunction)icd_vkCmdPushDescriptorSetWithTemplateKHR},
    {"vkCmdPushDescriptorSetWithTemplate2", (PFN_vkVoidFunction)icd_vkCmdPushDescriptorSetWithTemplateKHR},

    // Private data
    ENTRY(vkCreatePrivateDataSlot),
    ENTRY(vkDestroyPrivateDataSlot),
    ENTRY(vkSetPrivateData),
    ENTRY(vkGetPrivateData),

    // Sync
    ENTRY(vkCreateSemaphore),
    ENTRY(vkDestroySemaphore),
    ENTRY(vkCreateFence),
    ENTRY(vkDestroyFence),
    ENTRY(vkWaitForFences),
    ENTRY(vkResetFences),
    ENTRY(vkGetFenceStatus),
    ENTRY(vkQueueSubmit),
    ENTRY(vkQueueSubmit2),

    // Additional DXVK queries
    ENTRY(vkGetPhysicalDeviceExternalSemaphoreProperties),
    ENTRY(vkGetPhysicalDeviceSparseImageFormatProperties2),
    ENTRY(vkGetPhysicalDeviceSurfaceCapabilities2KHR),
    ENTRY(vkGetPhysicalDeviceSurfaceFormats2KHR),
    ENTRY(vkGetPhysicalDeviceWin32PresentationSupportKHR),
    ENTRY(vkCmdBeginDebugUtilsLabelEXT),
    ENTRY(vkCmdEndDebugUtilsLabelEXT),
    ENTRY(vkCmdInsertDebugUtilsLabelEXT),
    ENTRY(vkCreateDebugUtilsMessengerEXT),
    ENTRY(vkDestroyDebugUtilsMessengerEXT),
    ENTRY(vkSubmitDebugUtilsMessageEXT),
    ENTRY(vkGetPhysicalDeviceSurfacePresentModes2EXT),
    ENTRY(vkReleaseSwapchainImagesEXT),

    // Event / Query
    ENTRY(vkCreateEvent),
    ENTRY(vkDestroyEvent),
    ENTRY(vkCreateQueryPool),
    ENTRY(vkDestroyQueryPool),
    ENTRY(vkGetQueryPoolResults),
    ENTRY(vkResetQueryPool),
};

#undef ENTRY

// Generic stub — log when called so we know which unimplemented function DXVK invokes
// VK_NVX_image_view_handle: DXVK uses this to get ImageView handles for descriptor binding.
// We return the ICD-assigned imageView ID as the handle. Also triggers vkCreateImageView
// forwarding that DXVK would otherwise bypass.
static uint32_t VKAPI_CALL icd_vkGetImageViewHandleNVX(VkDevice, const void* pInfo) {
    // VkImageViewHandleInfoNVX: sType, pNext, imageView, descriptorType, sampler
    struct ImageViewHandleInfoNVX {
        VkStructureType sType; const void* pNext;
        VkImageView imageView; VkDescriptorType descriptorType; VkSampler sampler;
    };
    auto* info = static_cast<const ImageViewHandleInfoNVX*>(pInfo);
    return (uint32_t)(uint64_t)info->imageView; // return the ICD handle ID
}
static VkResult VKAPI_CALL icd_vkGetImageViewAddressNVX(VkDevice, VkImageView view, void* pProps) {
    // VkImageViewAddressPropertiesNVX: sType, pNext, deviceAddress, size
    struct ImageViewAddrProps { VkStructureType sType; void* pNext; VkDeviceAddress addr; VkDeviceSize size; };
    auto* props = static_cast<ImageViewAddrProps*>(pProps);
    props->addr = (uint64_t)view;
    props->size = 0;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL icd_generic_stub() {
    fprintf(stderr, "[ICD] !!! generic_stub called !!!\n");
    return VK_SUCCESS;
}

static PFN_vkVoidFunction lookupFunc(const char* pName) {
    for (const auto& e : g_funcTable) {
        if (strcmp(e.name, pName) == 0)
            return e.func;
    }

    // Try stripping KHR/EXT suffix and look up core version
    // e.g. "vkGetPhysicalDeviceFeatures2KHR" → "vkGetPhysicalDeviceFeatures2"
    std::string name(pName);
    for (const char* suffix : {"KHR", "EXT"}) {
        size_t slen = strlen(suffix);
        if (name.size() > slen && name.compare(name.size() - slen, slen, suffix) == 0) {
            std::string coreName = name.substr(0, name.size() - slen);
            for (const auto& e : g_funcTable) {
                if (coreName == e.name)
                    return e.func;
            }
        }
    }

    // Instance-level query functions: return nullptr (DXVK checks null = unsupported extension)
    // Device-level functions: return stub to prevent crashes during device init
    static const char* nullPrefixes[] = {
        "vkEnumeratePhysicalDevice",
        "vkGetPhysicalDeviceDisplay", "vkGetPhysicalDeviceVideo",
        "vkGetPhysicalDeviceCooperative", "vkGetPhysicalDeviceOptical",
        "vkGetPhysicalDeviceMultisample", "vkGetPhysicalDeviceCalibrate",
        "vkGetPhysicalDeviceExternal", "vkGetPhysicalDevicePresent",
        "vkGetPhysicalDeviceTool", "vkGetPhysicalDeviceFragment",
        "vkGetPhysicalDeviceSupported",
        "vkCreateDisplay", "vkCreateHeadless",
        "vkAcquireDrm", "vkAcquireWinrt",
        // Host image copy (Vulkan 1.4) — not supported, force DXVK to use CmdCopyBufferToImage
        "vkCopyMemoryToImage", "vkCopyImageToMemory", "vkCopyImageToImage",
        "vkTransitionImageLayout",
        nullptr
    };
    for (const char** p = nullPrefixes; *p; p++) {
        if (strncmp(pName, *p, strlen(*p)) == 0)
            return nullptr;
    }

    // Everything else: return stub (VK_SUCCESS / no-op)
    fprintf(stderr, "[ICD] Stubbed: %s\n", pName);
    return reinterpret_cast<PFN_vkVoidFunction>(icd_generic_stub);
}

// --- ICD entry points ---
extern "C" {

__declspec(dllexport) PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char* pName) {
    icdDbg(pName ? pName : "(null)");
    return lookupFunc(pName);
}

__declspec(dllexport) PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    return lookupFunc(pName);
}

__declspec(dllexport) VkResult VKAPI_CALL
vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pVersion) {
    icdDbg("NegotiateLoaderICDInterfaceVersion: enter");
    if (*pVersion > 5) *pVersion = 5;
    icdDbg("NegotiateLoaderICDInterfaceVersion: done");
    return VK_SUCCESS;
}

__declspec(dllexport) PFN_vkVoidFunction VKAPI_CALL
vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char* pName) {
    // For physical device commands, return our implementation
    return lookupFunc(pName);
}

} // extern "C"
