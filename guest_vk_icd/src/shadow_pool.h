#pragma once

// ShadowPool: centralized shadow memory for VkDeviceMemory.
// Reserves one large VA region at init, sub-allocates on demand.
// Single GetWriteWatch covers all pool-backed shadows per flush.
// Fallback: if pool is exhausted, individual VirtualAlloc is used.

#include <cstdint>
#include <cstdlib>
#include <vector>
#include <mutex>

#include <windows.h>

class ShadowPool {
public:
    ShadowPool() = default;
    ~ShadowPool() { destroy(); }

    ShadowPool(const ShadowPool&) = delete;
    ShadowPool& operator=(const ShadowPool&) = delete;

    // Initialize: reserve 'poolSize' bytes of VA space with MEM_WRITE_WATCH.
    // Pages are lazily committed on first alloc().
    bool init(uint64_t poolSize = DEFAULT_POOL_SIZE);

    // Release all reserved and committed pages.
    void destroy();

    // Allocate 'size' bytes (page-aligned) from pool. Returns nullptr on exhaustion.
    void* alloc(uint64_t size);

    // Free a pool-backed allocation. ptr must be within pool range.
    void free(void* ptr, uint64_t size);

    // True if ptr falls within [base_, base_ + poolSize_).
    bool contains(void* ptr) const;

    // Get dirty page offsets (relative to base_) for the committed range,
    // using GetWriteWatch with WRITE_WATCH_FLAG_RESET.
    // Returns count of dirty pages found.
    size_t getDirtyPages(std::vector<uint64_t>& outOffsets);

    void*    base()      const { return base_; }
    uint64_t committed() const { return committed_; }
    uint64_t poolSize()  const { return poolSize_; }

    // Default pool size (override via VBOXGPU_SHADOW_POOL_SIZE_MB env var)
    static constexpr uint64_t DEFAULT_POOL_SIZE =
        256ull * 1024 * 1024;  // 256 MB

private:
    void*    base_ = nullptr;
    uint64_t poolSize_ = 0;
    uint64_t committed_ = 0;   // bump allocator offset
    size_t   pageSize_ = 4096;
    std::mutex mutex_;

    struct FreeBlock {
        uint64_t offset;  // offset from base_
        uint64_t size;
    };
    std::vector<FreeBlock> freeList_;

    // Commit pages from committed_ up to neededEnd (page-aligned).
    bool commitUpTo(uint64_t neededEnd);

    // True if MEM_WRITE_WATCH was set on the full reservation.
    // If false, each commit must include MEM_WRITE_WATCH explicitly.
    bool wwOnReserve_ = false;
};
