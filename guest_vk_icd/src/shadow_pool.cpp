#include "shadow_pool.h"

#include <algorithm>
#include <cstdio>

// Log helper — writes to same ICD log file
static void poolDbg(const char* msg) {
    // Use same output as icdDbg() — stderr or CreateFile log
    fprintf(stderr, "[ShadowPool] %s\n", msg);
}

bool ShadowPool::init(uint64_t poolSize) {
    if (base_) return true; // already initialized

    std::lock_guard<std::mutex> lock(mutex_);

    // Read env var override
    const char* env = getenv("VBOXGPU_SHADOW_POOL_SIZE_MB");
    if (env) {
        long mb = atol(env);
        if (mb > 0 && mb <= 2048)
            poolSize = (uint64_t)mb * 1024 * 1024;
    }
    poolSize_ = poolSize;

    // Get system page size
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    pageSize_ = si.dwPageSize;

    // Reserve VA space with MEM_WRITE_WATCH so that all later
    // MEM_COMMIT on sub-ranges inherit write-watch tracking.
    base_ = VirtualAlloc(nullptr, (SIZE_T)poolSize_,
                         MEM_RESERVE | MEM_WRITE_WATCH, PAGE_NOACCESS);
    wwOnReserve_ = (base_ != nullptr);

    if (!base_) {
        // Retry without MEM_WRITE_WATCH on reserve — will add per-commit
        base_ = VirtualAlloc(nullptr, (SIZE_T)poolSize_,
                             MEM_RESERVE, PAGE_NOACCESS);
        wwOnReserve_ = false;
    }

    if (!base_) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "init FAILED: VirtualAlloc(MEM_RESERVE, %llu MB) failed (err=%lu)",
                 (unsigned long long)(poolSize_ / 1024 / 1024),
                 (unsigned long)GetLastError());
        poolDbg(buf);
        poolSize_ = 0;
        return false;
    }

    committed_ = 0;
    freeList_.clear();

    char buf[128];
    snprintf(buf, sizeof(buf),
             "init OK: reserved %llu MB at %p (wwOnReserve=%d)",
             (unsigned long long)(poolSize_ / 1024 / 1024),
             base_, (int)wwOnReserve_);
    poolDbg(buf);
    return true;
}

void ShadowPool::destroy() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (base_) {
        VirtualFree(base_, 0, MEM_RELEASE);
        base_ = nullptr;
    }
    poolSize_ = 0;
    committed_ = 0;
    freeList_.clear();
}

bool ShadowPool::commitUpTo(uint64_t neededEnd) {
    // Align up to page boundary
    uint64_t endPage = (neededEnd + pageSize_ - 1) & ~(uint64_t)(pageSize_ - 1);
    if (endPage <= committed_)
        return true;

    uint64_t commitSize = endPage - committed_;
    DWORD flags = MEM_COMMIT | MEM_WRITE_WATCH;

    void* addr = (uint8_t*)base_ + committed_;
    void* result = VirtualAlloc(addr, (SIZE_T)commitSize, flags, PAGE_READWRITE);
    if (!result) {
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "commitUpTo FAILED: VirtualAlloc(%p, %llu, MEM_COMMIT) err=%lu",
                 addr, (unsigned long long)commitSize, (unsigned long)GetLastError());
        poolDbg(buf);
        return false;
    }
    return true;
}

void* ShadowPool::alloc(uint64_t size) {
    if (size == 0) return nullptr;
    std::lock_guard<std::mutex> lock(mutex_);

    // Align to page
    uint64_t aligned = (size + pageSize_ - 1) & ~(uint64_t)(pageSize_ - 1);

    // First-fit search in free list (sorted by offset)
    for (size_t i = 0; i < freeList_.size(); i++) {
        if (freeList_[i].size < aligned) continue;
        uint64_t offset = freeList_[i].offset;
        uint64_t remain = freeList_[i].size - aligned;
        if (remain > 0) {
            freeList_[i].offset += aligned;
            freeList_[i].size = remain;
        } else {
            freeList_.erase(freeList_.begin() + i);
        }
        return (uint8_t*)base_ + offset;
    }

    // Bump allocate
    uint64_t offset = committed_;
    uint64_t newCommitted = committed_ + aligned;
    if (newCommitted > poolSize_)
        return nullptr; // pool exhausted — caller falls back to VirtualAlloc

    if (!commitUpTo(newCommitted))
        return nullptr;

    committed_ = newCommitted;
    return (uint8_t*)base_ + offset;
}

void ShadowPool::free(void* ptr, uint64_t size) {
    if (!ptr || !contains(ptr)) return;
    std::lock_guard<std::mutex> lock(mutex_);

    uint64_t offset = (uint8_t*)ptr - (uint8_t*)base_;
    uint64_t aligned = (size + pageSize_ - 1) & ~(uint64_t)(pageSize_ - 1);

    // Insert sorted by offset
    FreeBlock blk{ offset, aligned };
    auto it = freeList_.begin();
    while (it != freeList_.end() && it->offset < blk.offset)
        ++it;
    it = freeList_.insert(it, blk);

    // Merge adjacent blocks
    // Merge with previous
    if (it != freeList_.begin()) {
        auto prev = it - 1;
        if (prev->offset + prev->size == it->offset) {
            prev->size += it->size;
            freeList_.erase(it);
            it = prev;
        }
    }
    // Merge with next
    auto next = it + 1;
    if (next != freeList_.end() && it->offset + it->size == next->offset) {
        it->size += next->size;
        freeList_.erase(next);
    }
}

bool ShadowPool::contains(void* ptr) const {
    return ptr >= base_ && ptr < (uint8_t*)base_ + poolSize_;
}

size_t ShadowPool::getDirtyPages(std::vector<uint64_t>& outOffsets) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!base_ || committed_ == 0) return 0;

    ULONG_PTR maxPages = (ULONG_PTR)((committed_ + pageSize_ - 1) / pageSize_);
    std::vector<void*> pages(maxPages);
    ULONG_PTR count = maxPages;
    ULONG granularity = 0;

    UINT res = GetWriteWatch(WRITE_WATCH_FLAG_RESET,
                             base_, (SIZE_T)committed_,
                             pages.data(), &count, &granularity);
    if (res != 0 || count == 0) return 0;

    uintptr_t poolBase = (uintptr_t)base_;
    for (ULONG_PTR i = 0; i < count; i++)
        outOffsets.push_back((uintptr_t)pages[i] - poolBase);

    // GetWriteWatch returns pages in write-time order, not address order.
    // Caller uses std::lower_bound — must be sorted.
    std::sort(outOffsets.end() - count, outOffsets.end());

    return count;
}
