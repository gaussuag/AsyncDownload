# Phase 2: Collector Computation - Research

**Gathered:** 2026-03-24
**Status:** Research complete

## Research Topics Investigated

### 1. EMA Algorithm Implementation in C++

**Finding:** EMA (Exponential Moving Average) with α=0.8 per D-04.

**Implementation pattern:**
```cpp
// EMA formula: EMA_new = α * current_value + (1 - α) * EMA_old
// With α = 0.8: EMA_new = 0.8 * current + 0.2 * old
void TelemetryCollector::update_network_speed(uint64_t bytes, uint64_t timestamp_ns) {
    if (last_network_speed_ > 0) {
        double current_speed = static_cast<double>(bytes) * 1e9 / 
                              (timestamp_ns - last_network_timestamp_ns_);
        network_speed_ema_ = 0.8 * current_speed + 0.2 * network_speed_ema_;
    }
    last_network_timestamp_ns_ = timestamp_ns;
}
```

**Key considerations:**
- EMA state stored as member variable (not atomic per D-07 — consumer thread only)
- Use `double` for EMA values to avoid integer overflow
- Initial speed estimate needs handling (first sample sets EMA directly)

### 2. std::atomic Operations for Aggregation

**Finding:** Per D-17, use `std::atomic` for counters/flags. Consumer thread is single-threaded, so no mutex needed.

**Pattern for atomic aggregation:**
```cpp
// Direct atomic increment (for counts)
packets_enqueued_total_.fetch_add(1, std::memory_order_relaxed);

// Atomic update with computation
void TelemetryCollector::update_max_packet_size(size_t bytes) {
    size_t current_max = max_packet_size_bytes_.load(std::memory_order_relaxed);
    while (bytes > current_max && 
           !max_packet_size_bytes_.compare_exchange_weak(current_max, bytes,
               std::memory_order_relaxed, std::memory_order_relaxed)) {
        // CAS failed, current_max now holds the actual value — loop continues
    }
}

// Atomic flag for state tracking
std::atomic<bool> first_byte_received_set_{false};
```

**Key considerations:**
- Use `memory_order_relaxed` for pure aggregation (no synchronization needed)
- `compare_exchange_weak` for max updates (allows spurious failures)
- `load_value()` template already exists in `performance_metrics.hpp`

### 3. Thread-Safety Patterns (SPSC Scenario)

**Finding:** Single-producer/single-consumer (SPSC) — multiple producers (download threads), single consumer (collector thread).

**SPSC patterns:**
```cpp
// Copy-on-read for snapshot (D-12)
ProgressSnapshot TelemetryCollector::current_snapshot() const noexcept {
    ProgressSnapshot local = snapshot_;  // Atomic copy of entire struct
    local.watermark_timestamp_ns = std::chrono::steady_clock::now().time_since_epoch().count();
    return local;
}

// Lock-free queue consumption (already implemented in sink)
// moodycamel::BlockingConcurrentQueue handles thread-safety

// No mutex needed for aggregation state (consumer thread owns it)
```

**Key considerations:**
- Copy-on-read: entire `snapshot_` copied in one operation
- `watermark_timestamp_ns` set at read time (snapshot ID semantics, D-13)
- Producers never touch `snapshot_` or `summary_` directly

### 4. Copy-on-Read Snapshot Pattern

**Finding:** Per D-12 to D-14, current_snapshot() returns immediately without waiting.

**Implementation:**
```cpp
ProgressSnapshot TelemetryCollector::current_snapshot() const noexcept {
    // Copy current state atomically
    ProgressSnapshot snap = snapshot_;
    
    // D-13: watermark is the moment of snapshot creation
    snap.watermark_timestamp_ns = 
        std::chrono::steady_clock::now().time_since_epoch().count();
    
    return snap;  // RVO/move optimization
}
```

**Key considerations:**
- `snapshot_` is modified only by consumer thread — no lock needed
- Caller receives own copy — modifications don't affect ongoing reads
- No `wait_for_drain()` — returns immediately (D-14)

### 5. TTFB Calculation (D-01 to D-03)

**Finding:** TTFB stored only in `summary_`, computed on FirstByteReceived event.

**Implementation:**
```cpp
void TelemetryCollector::handle_event(const TelemetryEvent& event) noexcept {
    switch (event.type) {
        case TelemetryEventType::first_byte_received: {
            if (!first_byte_received_set_.load(std::memory_order_relaxed)) {
                uint64_t ttfb_ns = event.timestamp_ns - task_started_ns_;
                summary_.time_to_first_byte_ms = ttfb_ns / 1'000'000;
                first_byte_received_set_.store(true, std::memory_order_relaxed);
            }
            break;
        }
        // ... other events
    }
}
```

**Key considerations:**
- D-21: Ignore duplicate FirstByteReceived events
- D-01: TTFB goes to `summary_`, not `snapshot_`
- D-03: TTFB only available via `final_summary()`, not `current_snapshot()`

### 6. Final Summary Pre-computation (D-18 to D-20)

**Finding:** On TaskCompleted, pre-compute all summary fields from stored state.

**Implementation:**
```cpp
void TelemetryCollector::handle_event(const TelemetryEvent& event) noexcept {
    switch (event.type) {
        case TelemetryEventType::task_completed: {
            // D-18: Pre-compute summary on TaskCompleted
            summary_.max_memory_bytes = max_memory_bytes_;
            summary_.max_inflight_bytes = max_inflight_bytes_;
            summary_.total_pause_count = total_pause_count_;
            summary_.queue_full_pause_count = queue_full_pause_count_;
            summary_.packets_enqueued_total = packets_enqueued_total_;
            summary_.max_packet_size_bytes = max_packet_size_bytes_;
            
            // D-20: Derived metrics
            summary_.time_to_first_byte_ms = first_byte_received_ns_.has_value() 
                ? (*first_byte_received_ns_ - task_started_ns_) / 1'000'000 
                : 0;
            
            summary_.average_network_bytes_per_second = compute_avg_network_speed();
            summary_.average_disk_bytes_per_second = compute_avg_disk_speed();
            summary_.average_packet_size_bytes = compute_avg_packet_size();
            
            task_completed_.store(true, std::memory_order_relaxed);
            break;
        }
        // ... other events
    }
}
```

**Key considerations:**
- D-19: `final_summary()` recalculates from stored state (double-check)
- All fields from D-20 must be included
- TaskCompleted event triggers final snapshot computation

---

## Architecture Notes

### Member Variables Needed (per D-16)

```cpp
// TTFB tracking
uint64_t task_started_ns_{0};
std::optional<uint64_t> first_byte_received_ns_;
std::atomic<bool> first_byte_received_set_{false};

// Byte totals
std::atomic<std::size_t> total_download_bytes_{0};
std::atomic<std::size_t> total_persist_bytes_{0};

// Timestamp tracking for speed calculation
uint64_t last_network_timestamp_ns_{0};
uint64_t last_disk_timestamp_ns_{0};

// EMA speeds (double, not atomic — consumer thread only)
double network_speed_ema_{0.0};
double disk_speed_ema_{0.0};

// Packet statistics
std::atomic<std::size_t> packets_enqueued_total_{0};
std::atomic<std::size_t> total_packet_bytes_{0};  // For avg calculation
std::atomic<std::size_t> max_packet_size_bytes_{0};

// Pause tracking
std::atomic<std::size_t> total_pause_count_{0};
std::atomic<std::size_t> queue_full_pause_count_{0};

// Peak tracking
std::atomic<std::size_t> max_memory_bytes_{0};
std::atomic<std::size_t> max_inflight_bytes_{0};

// State flags
std::atomic<bool> task_completed_{false};
```

### Event Handling Flow

1. **TaskStarted**: Record `task_started_ns_`, reset all state
2. **FirstByteReceived**: Compute TTFB, set flag
3. **DownloadDelta**: Update bytes, timestamps, speeds (EMA), packet stats
4. **PersistDelta**: Update bytes, timestamps, disk speed (EMA)
5. **QueuePaused**: Increment pause counters
6. **MemorySample**: Update max_memory_bytes
7. **TaskCompleted**: Pre-compute entire summary

### Error Handling (D-21 to D-23)

- **Duplicate TaskStarted**: Keep first timestamp, ignore subsequent
- **Events after TaskCompleted**: Silently ignore
- **Out-of-order events**: Silently ignore (e.g., DownloadDelta before TaskStarted)

---

## Implementation Strategy

### Wave 1: Core Members + TTFB + Packet Stats
- Add all member variables to `telemetry_collector.hpp`
- Implement TTFB calculation
- Implement packet statistics (count, avg, max)

### Wave 2: Speed Calculation + Peaks
- Implement EMA-based network/disk speed
- Implement max_memory_bytes and max_inflight_bytes tracking
- Implement pause count tracking

### Wave 3: Snapshot + Final Summary
- Implement `current_snapshot()` with copy-on-read
- Implement `final_summary()` recalculation
- Handle TaskCompleted pre-computation

### Wave 4: Integration Tests
- Unit tests for each aggregation path
- Verify snapshot consistency
- Verify final summary correctness

---

*Research completed: 2026-03-24*
