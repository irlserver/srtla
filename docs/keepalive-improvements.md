# Keepalive-Based Algorithm Improvements

## Overview

This document tracks the implementation of improvements to SRTLA's load balancing and quality evaluation algorithms by leveraging connection information from extended keepalive packets.

## Current State

### Extended Keepalive Protocol

The codebase already supports extended keepalive packets that include rich connection telemetry from the sender:

```c
typedef struct __attribute__((__packed__)) {
  uint32_t conn_id;
  int32_t window;                    // SRT window size
  int32_t in_flight;                 // Packets currently in flight
  uint32_t rtt_ms;                   // Round-trip time in milliseconds
  uint32_t nak_count;                // NAK (retransmission) count
  uint32_t bitrate_bytes_per_sec;    // Client-side bitrate measurement
} connection_info_t;
```

**Packet Length**: 38 bytes (extended keepalive)

**Previous Status**: This data was only parsed and logged, not used for decision-making.

**Current Status**: **FULLY IMPLEMENTED** - All telemetry data is now stored and used for quality assessment.

**Location**: `src/protocol/srtla_handler.cpp` (handler), `src/quality/quality_evaluator.cpp` (evaluation)

## Proposed Improvements

### Phase 1: RTT-Based Quality Assessment (HIGH PRIORITY)

**Rationale**: Latency is often a better early indicator of connection problems than bandwidth. High or increasing RTT signals congestion, routing issues, or link instability.

**Implementation**:
- Store RTT values in `ConnectionStats`
- Track RTT history for trend analysis
- Add error points based on RTT thresholds
- Monitor RTT variance (jitter)

**Error Point Thresholds**:
- RTT > 500ms: +20 error points
- RTT > 200ms: +10 error points
- RTT > 100ms: +5 error points
- High RTT variance: +10 error points

**Status**: NOT STARTED

### Phase 2: NAK Count Validation (HIGH PRIORITY)

**Rationale**: The sender's NAK count provides ground truth about packet loss and retransmissions, which is more accurate than receiver-side estimation.

**Implementation**:
- Store sender NAK count in `ConnectionStats`
- Compare with receiver-side packet loss tracking
- Use NAK rate (NAKs per packet) for quality scoring
- Replace or supplement current loss detection

**Error Point Thresholds**:
- NAK rate > 20%: +40 error points
- NAK rate > 10%: +20 error points
- NAK rate > 5%: +10 error points
- NAK rate > 1%: +5 error points

**Status**: NOT STARTED

### Phase 3: Window Utilization Analysis (MEDIUM PRIORITY)

**Rationale**: The ratio of `in_flight/window` reveals how aggressively the sender is using each connection and can indicate congestion or throttling.

**Implementation**:
- Calculate window utilization ratio
- Detect persistently full windows (congestion)
- Detect low utilization (client-side issues)
- Use for advanced load balancing decisions

**Analysis**:
- Utilization > 95%: Possible congestion, reduce priority
- Utilization < 30%: Client throttling, investigate
- Optimal range: 60-80% utilization

**Status**: NOT STARTED

### Phase 4: Sender Bitrate Validation (LOW PRIORITY)

**Rationale**: Comparing sender and receiver bitrate measurements can detect path issues and validate metrics.

**Implementation**:
- Store sender bitrate in `ConnectionStats`
- Compare sender vs receiver measurements
- Alert on significant discrepancies (>20% difference)
- Use for debugging and diagnostics

**Status**: ✅ **COMPLETED** (2025-12-04)

## Implementation Plan (COMPLETED)

### Step 1: Data Structure Updates ✅
- [x] Add keepalive metrics fields to `ConnectionStats` (receiver_config.h)
  - `uint32_t rtt_ms`
  - `uint32_t rtt_history[RTT_HISTORY_SIZE]`
  - `uint8_t rtt_history_idx`
  - `time_t last_keepalive`
  - `int32_t window`
  - `int32_t in_flight`
  - `uint32_t sender_nak_count`
  - `uint32_t last_sender_nak_count`
  - `uint32_t sender_bitrate_bps`

### Step 2: Keepalive Handler Updates ✅
- [x] Modify `SRTLAHandler::handle_keepalive()` to store metrics
- [x] Update connection stats with keepalive data
- [x] Track timestamp of last keepalive received
- [x] Add helper functions for RTT history and variance

### Step 3: Quality Evaluator Enhancements ✅
- [x] Add RTT-based error point calculation
- [x] Add NAK rate error point calculation
- [x] Add window utilization analysis
- [x] Add bitrate comparison logic

### Step 4: Testing and Validation ⏳
- [ ] Test with simulated high-latency connections
- [ ] Test with varying packet loss scenarios
- [ ] Validate error point calculations
- [ ] Monitor impact on load balancing behavior

### Step 5: Documentation ✅
- [x] Update keepalive-improvements.md with implementation details
- [x] Document keepalive metrics in technical docs
- [x] Add configuration parameters
- [ ] Update README.md with new quality metrics

## Expected Benefits

1. **Earlier Problem Detection**: RTT increases often precede bandwidth degradation
2. **More Accurate Loss Tracking**: Sender NAK count is ground truth
3. **Better Load Distribution**: Window utilization reveals true connection capacity
4. **Improved Debugging**: Bitrate comparison helps diagnose path issues
5. **Reduced Latency**: Penalizing high-RTT connections improves stream responsiveness

## Configuration Parameters

New parameters to add:

```cpp
// RTT thresholds (milliseconds)
inline constexpr uint32_t RTT_THRESHOLD_CRITICAL = 500;  // 500ms
inline constexpr uint32_t RTT_THRESHOLD_HIGH = 200;      // 200ms
inline constexpr uint32_t RTT_THRESHOLD_MODERATE = 100;  // 100ms

// Window utilization thresholds
inline constexpr double WINDOW_UTILIZATION_CONGESTED = 0.95;
inline constexpr double WINDOW_UTILIZATION_LOW = 0.30;

// Bitrate comparison tolerance
inline constexpr double BITRATE_DISCREPANCY_THRESHOLD = 0.20;  // 20%

// RTT variance threshold for jitter detection
inline constexpr uint32_t RTT_VARIANCE_THRESHOLD = 50;  // 50ms stddev
```

## Risks and Mitigations

### Risk: Keepalive packets might not arrive regularly
- **Mitigation**: Only apply RTT-based penalties if keepalive received within last 2 seconds
- **Mitigation**: Fall back to receiver-side metrics if keepalives stale

### Risk: Sender-side metrics might be inaccurate
- **Mitigation**: Use as supplementary data, not sole decision factor
- **Mitigation**: Validate against receiver measurements

### Risk: Too aggressive RTT penalties might exclude viable connections
- **Mitigation**: Use gradual error point increases, not binary decisions
- **Mitigation**: Maintain grace period for new connections

## Progress Tracking

- **Phase 1 (RTT)**: ✅ 100% complete
- **Phase 2 (NAK)**: ✅ 100% complete  
- **Phase 3 (Window)**: ✅ 100% complete
- **Phase 4 (Bitrate)**: ✅ 100% complete

**Overall Progress**: ✅ 100% (Implementation Complete)

**Implementation Date**: 2025-12-04
**Build Status**: ✅ Successful
**Next Steps**: Testing and validation
