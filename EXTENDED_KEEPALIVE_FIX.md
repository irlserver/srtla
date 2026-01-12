# Extended Keepalive Feedback Loop Fix

## Problem

When using srtla_send with extended keepalives (38-byte keepalives with connection_info_t), 
one connection would drop to 0 bandwidth and never recover, while the other connection 
carried 100% of the traffic. This did NOT occur with vanilla srtla_send (minimal 2-byte keepalives).

## Root Cause Analysis

### The Feedback Loop

1. **Initial state**: Both connections share traffic load
2. **Minor network event**: One connection experiences slight degradation (e.g., packet loss)
3. **Client reduces usage**: Sender uses the degraded connection less
4. **Connection becomes idle**: Idle connections send extended keepalives (by design)
5. **Receiver measures 0 bandwidth**: Since connection is idle, receiver-side bandwidth measurement = 0
6. **Heavy bandwidth penalty**: Receiver applies 40 error points for performance_ratio < 0.3
7. **ACK throttling**: 40+ error points → WEIGHT_CRITICAL → 20% ACK throttle
8. **Client further reduces usage**: Fewer ACKs → lower window growth → connection scored poorly
9. **Permanent 0 bandwidth**: Connection locked at 0, never recovers

### Why It Only Happens with Extended Keepalives

- **Legacy senders (minimal keepalives)**: Idle connections don't provide telemetry, so receiver 
  can't distinguish them as clearly. Bandwidth penalties apply but without the enhanced evaluation,
  the feedback loop is less severe.

- **Extended keepalives**: Idle connections send full telemetry, triggering "full evaluation mode".
  Receiver confidently applies aggressive bandwidth penalties, creating a strong feedback loop.

## Solution

### 1. Lighter Bandwidth Penalties for Connections with Telemetry

**File**: `src/quality/quality_evaluator.cpp:175-203`

For connections WITH sender telemetry (extended keepalives):
- Reduce bandwidth penalty from 40 → 10 points (for performance_ratio < 0.3)
- Reduce other tiers proportionally
- Rely more on telemetry metrics (RTT, NAK rate, window utilization) as primary indicators

For connections WITHOUT telemetry (legacy senders):
- Keep original aggressive penalties (40 points for < 0.3)
- Bandwidth remains the primary quality indicator

**Rationale**: 
- Bandwidth penalties create feedback loops with ACK throttling
- When we have telemetry, we can use more direct quality indicators (packet loss, RTT, NAKs)
- Legacy senders need bandwidth penalties as they lack alternative quality signals

### 2. Recovery Boost for Throttled Connections

**File**: `src/quality/load_balancer.cpp:86-96`

For connections with recent telemetry that are heavily throttled (<50%) but show improvement
(error points < 15):
- Apply a 15% throttle boost (up to 60% max)
- This helps connections escape the feedback loop when network quality improves

Only applies to connections with sender telemetry. Legacy senders don't get this boost.

**Rationale**:
- Breaks the feedback loop: low throttle → low usage → low bandwidth → low throttle
- Only applies when connection has actually improved (error points dropped)
- Conservative boost (15%) prevents over-correction

## Expected Behavior After Fix

### With Extended Keepalives (srtla_send)

**Before**:
```
[::ffff:51973] BW: 7469 kbps, Loss: 0%, Error: 0, Weight: 100%, Throttle: 1.00
[::ffff:47884] BW: 0 kbps, Loss: 0%, Error: 40, Weight: 10%, Throttle: 0.20  ← STUCK
```

**After**:
```
[::ffff:51973] BW: 7200 kbps, Loss: 0%, Error: 0, Weight: 100%, Throttle: 1.00
[::ffff:47884] BW: 300 kbps, Loss: 0%, Error: 10, Weight: 70%, Throttle: 0.70  ← RECOVERED
```

Idle connections get lower error points (10 instead of 40), enabling them to participate 
in load balancing when they receive traffic again.

### With Legacy Keepalives (vanilla srtla_send)

**Behavior unchanged** - legacy senders continue to use original bandwidth penalty logic
since they lack alternative quality signals.

## Technical Details

### Bandwidth Penalty Comparison

| Performance Ratio | Legacy Senders | With Telemetry |
|-------------------|----------------|----------------|
| < 0.3             | 40 points      | 10 points      |
| 0.3 - 0.5         | 25 points      | 7 points       |
| 0.5 - 0.7         | 15 points      | 4 points       |
| 0.7 - 0.85        | 5 points       | 2 points       |

### Recovery Boost Logic

```cpp
if (has_recent_telemetry && old_throttle < 0.5 && error_points < 15) {
    new_throttle = min(new_throttle + 0.15, 0.6);
}
```

Conditions:
1. Connection must have sent extended keepalives recently
2. Current throttle must be below 50% (heavily throttled)
3. Error points must be below 15 (showing improvement)

Result: Throttle boosted by 15%, capped at 60%

## Testing

Test the fix by:

1. **Extended keepalive scenario**:
   - Use srtla_send with extended keepalives
   - Verify both connections participate in load balancing
   - Temporarily degrade one connection (artificial packet loss)
   - Verify connection recovers when packet loss stops

2. **Legacy scenario**:
   - Use vanilla srtla_send (minimal keepalives)
   - Verify behavior is unchanged from before
   - Confirm aggressive bandwidth penalties still apply

## Files Modified

- `src/quality/quality_evaluator.cpp`: Conditional bandwidth penalties
- `src/quality/load_balancer.cpp`: Recovery boost for throttled connections

## Backward Compatibility

✅ **Fully backward compatible**

- Legacy senders: No behavioral change
- Extended keepalives: Fixed feedback loop issue
- No protocol changes
- No configuration changes needed
