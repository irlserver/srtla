# Technical Design: Keepalive-Based Quality Metrics

## Architecture Overview

This document describes the technical architecture for integrating keepalive connection telemetry into SRTLA's quality evaluation and load balancing systems.

## Current Architecture

### Data Flow

```
Sender (srtla_send)
  |
  | Extended KEEPALIVE packet with connection_info_t
  |
  v
SRTLAHandler::handle_keepalive()
  |
  | parse_keepalive_conn_info()
  |
  v
Log telemetry (CURRENT: data is discarded)
```

### Existing Quality Evaluation

```
QualityEvaluator::evaluate_group()
  |
  +-- Calculate bandwidth from bytes_received delta
  +-- Calculate packet loss from packets_lost delta
  +-- Assign error points based on:
      - Performance ratio (bandwidth vs expected)
      - Packet loss ratio
  +-- Calculate weight_percent
  +-- Apply ACK throttling based on weight
```

## Proposed Architecture

### Enhanced Data Flow

```
Sender (srtla_send)
  |
  | Extended KEEPALIVE packet with connection_info_t
  |
  v
SRTLAHandler::handle_keepalive()
  |
  | parse_keepalive_conn_info()
  |
  +-- Store in ConnectionStats:
      - rtt_us
      - window
      - in_flight
      - sender_nak_count
      - sender_bitrate_bps
      - last_keepalive timestamp
  |
  +-- Update RTT history buffer
  |
  v
Connection object (updated with fresh telemetry)
```

### Enhanced Quality Evaluation

```
QualityEvaluator::evaluate_group()
  |
  +-- Existing metrics:
  |   - Bandwidth
  |   - Packet loss
  |
  +-- NEW: RTT-based metrics:
  |   - Check keepalive staleness
  |   - Calculate RTT error points
  |   - Calculate RTT variance (jitter)
  |
  +-- NEW: NAK-based metrics:
  |   - Calculate NAK rate
  |   - Validate against receiver loss
  |
  +-- NEW: Window utilization:
  |   - Calculate in_flight/window ratio
  |   - Detect congestion
  |   - Detect throttling
  |
  +-- NEW: Bitrate validation:
  |   - Compare sender vs receiver bitrate
  |   - Log discrepancies
  |
  +-- Aggregate all error points
  +-- Calculate weight_percent
  +-- Apply ACK throttling
```

## Data Structures

### Enhanced ConnectionStats

```cpp
struct ConnectionStats {
    // Existing receiver-side metrics
    uint64_t bytes_received = 0;
    uint64_t packets_received = 0;
    uint32_t packets_lost = 0;
    uint64_t last_eval_time = 0;
    uint64_t last_bytes_received = 0;
    uint64_t last_packets_received = 0;
    uint32_t last_packets_lost = 0;
    uint32_t error_points = 0;
    uint8_t weight_percent = WEIGHT_FULL;
    uint64_t last_ack_sent_time = 0;
    double ack_throttle_factor = 1.0;
    uint16_t nack_count = 0;
    
    // NEW: Sender-side telemetry from keepalive
    uint64_t rtt_us = 0;
    uint64_t rtt_history[5] = {0};
    uint8_t rtt_history_idx = 0;
    time_t last_keepalive = 0;
    
    int32_t window = 0;
    int32_t in_flight = 0;
    
    uint32_t sender_nak_count = 0;
    uint32_t last_sender_nak_count = 0;
    
    uint32_t sender_bitrate_bps = 0;
};
```

## Component Details

### RTT Tracking and Analysis

#### RTT History Buffer

Use a circular buffer to track the last 5 RTT measurements:

```cpp
void update_rtt_history(ConnectionStats &stats, uint64_t rtt) {
    stats.rtt_history[stats.rtt_history_idx] = rtt;
    stats.rtt_history_idx = (stats.rtt_history_idx + 1) % 5;
    stats.rtt_us = rtt;  // Store most recent
}
```

#### RTT Variance Calculation

Calculate standard deviation to detect jitter:

```cpp
double calculate_rtt_variance(const ConnectionStats &stats) {
    // Count valid samples
    int count = 0;
    double sum = 0;
    for (int i = 0; i < 5; i++) {
        if (stats.rtt_history[i] > 0) {
            sum += stats.rtt_history[i];
            count++;
        }
    }
    
    if (count < 2) return 0;  // Need at least 2 samples
    
    double mean = sum / count;
    double variance_sum = 0;
    for (int i = 0; i < 5; i++) {
        if (stats.rtt_history[i] > 0) {
            double diff = stats.rtt_history[i] - mean;
            variance_sum += diff * diff;
        }
    }
    
    return sqrt(variance_sum / count);
}
```

#### RTT Error Points

```cpp
uint32_t calculate_rtt_error_points(const ConnectionStats &stats, time_t current_time) {
    // Don't use stale keepalive data
    if (current_time - stats.last_keepalive > KEEPALIVE_STALENESS_THRESHOLD) {
        return 0;
    }
    
    uint32_t points = 0;
    
    // Base RTT penalties
    if (stats.rtt_us > RTT_THRESHOLD_CRITICAL) {
        points += 20;
    } else if (stats.rtt_us > RTT_THRESHOLD_HIGH) {
        points += 10;
    } else if (stats.rtt_us > RTT_THRESHOLD_MODERATE) {
        points += 5;
    }
    
    // Jitter penalty
    double variance = calculate_rtt_variance(stats);
    if (variance > RTT_VARIANCE_THRESHOLD) {
        points += 10;
    }
    
    return points;
}
```

### NAK Rate Analysis

#### NAK Rate Calculation

```cpp
uint32_t calculate_nak_error_points(ConnectionStats &stats, uint64_t packets_diff) {
    if (packets_diff == 0) return 0;
    
    uint32_t nak_diff = stats.sender_nak_count - stats.last_sender_nak_count;
    double nak_rate = static_cast<double>(nak_diff) / packets_diff;
    
    uint32_t points = 0;
    if (nak_rate > NAK_RATE_CRITICAL) {
        points += 40;
    } else if (nak_rate > NAK_RATE_HIGH) {
        points += 20;
    } else if (nak_rate > NAK_RATE_MODERATE) {
        points += 10;
    } else if (nak_rate > NAK_RATE_LOW) {
        points += 5;
    }
    
    stats.last_sender_nak_count = stats.sender_nak_count;
    return points;
}
```

### Window Utilization

#### Utilization Analysis

```cpp
uint32_t calculate_window_error_points(const ConnectionStats &stats) {
    if (stats.window <= 0) return 0;
    
    double utilization = static_cast<double>(stats.in_flight) / stats.window;
    
    uint32_t points = 0;
    
    // Persistently full window indicates congestion
    if (utilization > WINDOW_UTILIZATION_CONGESTED) {
        points += 15;
    }
    
    // Very low utilization might indicate client-side throttling
    // This is informational, not necessarily bad
    if (utilization < WINDOW_UTILIZATION_LOW) {
        // Log for debugging but don't penalize
    }
    
    return points;
}
```

### Bitrate Validation

#### Discrepancy Detection

```cpp
void validate_bitrate(const ConnectionStats &stats, 
                     double receiver_bitrate_bps,
                     const struct sockaddr *addr) {
    if (stats.sender_bitrate_bps == 0) return;
    
    double ratio = std::abs(receiver_bitrate_bps - stats.sender_bitrate_bps) 
                   / stats.sender_bitrate_bps;
    
    if (ratio > BITRATE_DISCREPANCY_THRESHOLD) {
        spdlog::warn("[{}:{}] Large bitrate discrepancy: "
                     "sender={} bps, receiver={} bps ({}%)",
                     print_addr(addr), port_no(addr),
                     stats.sender_bitrate_bps,
                     static_cast<uint64_t>(receiver_bitrate_bps),
                     ratio * 100);
    }
}
```

## Integration Points

### 1. SRTLAHandler::handle_keepalive()

**Before**:
```cpp
void SRTLAHandler::handle_keepalive(...) {
    connection_info_t info;
    if (parse_keepalive_conn_info(..., &info)) {
        // Log only
        spdlog::info("Uplink telemetry: ...");
    }
    // Echo keepalive back
}
```

**After**:
```cpp
void SRTLAHandler::handle_keepalive(...) {
    connection_info_t info;
    if (parse_keepalive_conn_info(..., &info)) {
        // Log telemetry
        spdlog::info("Uplink telemetry: ...");
        
        // NEW: Store in connection stats
        update_connection_telemetry(conn, info, current_time);
    }
    // Echo keepalive back
}

void update_connection_telemetry(ConnectionPtr conn, 
                                 const connection_info_t &info,
                                 time_t current_time) {
    auto &stats = conn->stats();
    
    // Update RTT with history
    update_rtt_history(stats, info.rtt_us);
    
    // Update window metrics
    stats.window = info.window;
    stats.in_flight = info.in_flight;
    
    // Update NAK count
    stats.sender_nak_count = info.nak_count;
    
    // Update bitrate
    stats.sender_bitrate_bps = info.bitrate_bytes_per_sec;
    
    // Mark keepalive timestamp
    stats.last_keepalive = current_time;
}
```

### 2. QualityEvaluator::evaluate_group()

**Modify existing evaluation loop**:

```cpp
void QualityEvaluator::evaluate_group(...) {
    // ... existing bandwidth/loss calculation ...
    
    for (std::size_t idx = 0; idx < bandwidth_info.size(); ++idx) {
        auto conn = group->connections()[idx];
        
        // ... existing error point calculation ...
        
        // NEW: Add RTT-based error points
        conn->stats().error_points += 
            calculate_rtt_error_points(conn->stats(), current_time);
        
        // NEW: Add NAK-based error points
        conn->stats().error_points += 
            calculate_nak_error_points(conn->stats(), packets_diff);
        
        // NEW: Add window utilization error points
        conn->stats().error_points += 
            calculate_window_error_points(conn->stats());
        
        // NEW: Validate bitrate (logging only)
        validate_bitrate(conn->stats(), 
                        bandwidth_info[idx].bandwidth_kbits_per_sec * 125,
                        &conn->address());
        
        // ... rest of existing evaluation ...
    }
}
```

## Error Point Budget

Total maximum error points: **~100 points**

| Source | Max Points | Thresholds |
|--------|------------|------------|
| Bandwidth performance | 40 | <30% of expected |
| Packet loss (existing) | 40 | >20% loss |
| RTT | 20 | >500ms |
| RTT variance (jitter) | 10 | >50ms stddev |
| NAK rate | 40 | >20% |
| Window congestion | 15 | >95% utilization |

**Note**: Multiple metrics can contribute simultaneously, but weight calculation will clamp the final result.

## Weight Calculation

Existing weight levels remain unchanged:

```cpp
if (error_points <= 5) weight = WEIGHT_FULL;          // 100%
else if (error_points <= 15) weight = WEIGHT_EXCELLENT; // 85%
else if (error_points <= 30) weight = WEIGHT_DEGRADED;  // 70%
else if (error_points <= 45) weight = WEIGHT_FAIR;      // 55%
else if (error_points <= 60) weight = WEIGHT_POOR;      // 40%
else weight = WEIGHT_CRITICAL;                          // 10%
```

## Backward Compatibility

The implementation maintains backward compatibility:

1. **Graceful degradation**: If keepalive packets don't include extended info, only receiver-side metrics are used
2. **Staleness detection**: RTT metrics ignored if keepalive is >2 seconds old
3. **No breaking changes**: All changes are additive to `ConnectionStats`

## Performance Considerations

### Memory Overhead

Per connection:
- RTT history: 5 × 8 bytes = 40 bytes
- New fields: ~32 bytes
- Total: ~72 bytes per connection

For 16 connections × 200 groups = 3200 connections max:
- Additional memory: ~225 KB (negligible)

### CPU Overhead

- RTT variance calculation: O(1) with fixed 5-sample buffer
- All new calculations: O(1) per connection
- Performed once per `CONN_QUALITY_EVAL_PERIOD` (5 seconds)
- Expected impact: <1% CPU increase

## Testing Strategy

### Unit Tests

- [ ] RTT history buffer wrap-around
- [ ] RTT variance calculation with edge cases
- [ ] NAK rate calculation
- [ ] Window utilization ratio
- [ ] Bitrate discrepancy detection
- [ ] Staleness detection

### Integration Tests

- [ ] Keepalive data correctly stored
- [ ] Error points correctly calculated
- [ ] Weight correctly updated
- [ ] ACK throttling responds to RTT changes
- [ ] Graceful degradation without extended keepalives

### System Tests

- [ ] Multi-connection load balancing
- [ ] Connection failover with RTT spikes
- [ ] Recovery after network issues
- [ ] Performance with 16 connections
- [ ] Memory leak detection
- [ ] Long-running stability (24+ hours)

---

**Status**: Design Complete  
**Implementation**: Not Started  
**Last Updated**: 2025-12-04
