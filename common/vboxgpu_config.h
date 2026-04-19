#pragma once

// ---------------------------------------------------------------------------
// Dirty tracking (MEM_WRITE_WATCH) threshold
//
// VBOXGPU_DIRTY_TRACK_SIZE_LIMIT: mapped regions (and their backing shadows)
// larger than this value are skipped in Phase 2 of flushMappedMemory — no
// incremental WriteMemory is sent for them via dirty tracking.
//
// At 256 MB this covers virtually all HOST_VISIBLE allocations that DXVK
// sub-allocates from (upload heap, readback heap, etc.).
//
// Decrease to reduce the page-scan work per flush; increase to cover larger
// heaps.  Shadows above the limit still have their dirty bits reset via
// ResetWriteWatch to prevent stale accumulation — they rely on the explicit
// flushBufferRange path (CopyBuffer, CopyBufferToImage, UpdateDescriptorSets).
// ---------------------------------------------------------------------------
#define VBOXGPU_DIRTY_TRACK_SIZE_LIMIT  (256u * 1024u * 1024u)
