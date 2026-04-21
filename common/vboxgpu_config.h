#pragma once

// ---------------------------------------------------------------------------
// Dirty tracking (MEM_WRITE_WATCH) thresholds
//
// DIRTY_TRACK_MAX_SHADOW_SIZE: shadows larger than this are NOT incrementally
// dirty-tracked.  Their dirty bits are still reset each flush (via
// ResetWriteWatch) to prevent stale accumulation — large buffers rely on
// explicit flushBufferRange calls instead.
//
// DIRTY_TRACK_MAX_REGION_SIZE: mapped regions larger than this are skipped in
// Phase 2 (no WriteMemory sent for them via dirty tracking).
//
// Both values should be identical.  Adjust to trade off bandwidth vs. latency:
//   smaller  → less spurious WriteMemory traffic, relies more on flushBufferRange
//   larger   → more incremental coverage, but risks large stale flushes if
//              previously-excluded shadows are newly included mid-session
// ---------------------------------------------------------------------------
#define VBOXGPU_DIRTY_TRACK_SIZE_LIMIT  (256u * 1024u * 1024u)  // Cover all HOST_VISIBLE allocations with GetWriteWatch
