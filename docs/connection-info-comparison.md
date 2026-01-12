# Connection Info Algorithm: Real-Time Comparison Guide

## Overview

This implementation allows you to compare the **Connection Info algorithm** (with sender telemetry) against the **Legacy algorithm** (receiver-side metrics only) **in real-time on the same data stream**.

Both algorithms run simultaneously on every connection evaluation, so you can see how they differ under identical network conditions without needing to replicate setups.

## How It Works

The system runs **both algorithms in parallel**:

1. **Connection Info Algorithm** (NEW): Uses extended telemetry from keepalive packets
   - RTT measurements from sender
   - Window size and in-flight packets
   - Sender NAK count
   - Sender bitrate
   - Receiver bandwidth and packet loss

2. **Legacy Algorithm** (OLD): Uses only receiver-side measurements
   - Receiver bandwidth (calculated from received bytes)
   - Receiver packet loss rate
   - No RTT, window, or sender NAK data

Both algorithms produce:
- Error points (quality assessment)
- Weight percentage (connection quality: 100% = best, 10% = worst)
- ACK throttle factor (load balancing control: 1.0 = no throttling, 0.2 = minimum)

## Comparison Mode Flag

The comparison mode is controlled in `src/receiver_config.h:13-15`:

```cpp
#define ENABLE_ALGO_COMPARISON 1  // Enable comparison (BOTH algorithms run)
#define ENABLE_ALGO_COMPARISON 0  // Disable comparison (production mode)
```

**Default: ENABLED** for development and testing.

## Log Output

### Keepalive Packet Logs (Always Shown)

Every keepalive with connection info logs the detailed telemetry:

```
[INFO] [192.168.1.100:5000] [Group: 0x...] Per-connection keepalive: ID=0, BW: 2500.00 kbits/s, Window=8192, In-flight=120, RTT=45ms, NAKs=3
```

### Algorithm Comparison Logs (When Enabled)

When algorithms **disagree** (weight delta ≥ 5% OR error points delta ≥ 5), you'll see:

```
[INFO] [192.168.1.100:5000] [ALGO_CMP] ConnInfo: Err=15 W=70% T=0.70 | Legacy: Err=5 W=85% T=0.85 | Delta: E=+10 W=-15% T=-0.15
```

This shows:
- **ConnInfo**: Connection Info algorithm results (uses sender telemetry)
- **Legacy**: Legacy algorithm results (receiver-side only)
- **Delta**: Difference (positive = ConnInfo more pessimistic, negative = Legacy more pessimistic)

When algorithms **agree** (within 5% threshold), only debug logging occurs to reduce spam.

### Load Balancer Adjustment Logs

Every 5 seconds (or when quality changes), you'll see side-by-side comparison:

```
[INFO] [Group: 0x...] Connection parameters adjusted:
[INFO] [192.168.1.100:5000] [COMPARISON] ConnInfo: Weight=70%, Throttle=0.70, ErrPts=15 | Legacy: Weight=85%, Throttle=0.85, ErrPts=5 | Delta: W=-15%, T=-0.15, E=+10
```

This shows the final decisions from both algorithms for all connections.

## What the Deltas Mean

### Error Points Delta

- **Positive (+)**: Connection Info algorithm is **more pessimistic** (detected more issues)
  - Likely due to RTT problems, NAK rate, or window congestion not visible to legacy
- **Negative (-)**: Legacy algorithm is **more pessimistic**
  - Unusual; could happen if receiver sees packet loss that sender hasn't reported yet
- **Zero or small**: Both algorithms see similar connection quality

### Weight Delta

- **Positive (+)**: Connection Info gives **higher weight** (more optimistic)
  - Rare; would indicate legacy is penalizing incorrectly
- **Negative (-)**: Connection Info gives **lower weight** (more pessimistic)
  - Common; Connection Info detects RTT/NAK/window issues legacy misses
- **Zero or small**: Both algorithms agree on connection quality

### Throttle Delta

- **Positive (+)**: Connection Info throttles **less** (more aggressive ACKs)
- **Negative (-)**: Connection Info throttles **more** (fewer ACKs, shifts load away)
- Follows weight delta (throttle = max(0.2, weight/100))

## Key Differences Between Algorithms

| Metric | Connection Info Algorithm | Legacy Algorithm |
|--------|--------------------------|------------------|
| **Bandwidth** | ✅ Receiver calculated | ✅ Receiver calculated |
| **Packet Loss** | ✅ Receiver detected | ✅ Receiver detected |
| **RTT** | ✅ Sender measurement | ❌ Not available |
| **RTT Variance** | ✅ Tracked (jitter penalty) | ❌ Not available |
| **Window Utilization** | ✅ Window/in-flight ratio | ❌ Not available |
| **Sender NAK Rate** | ✅ Sender-reported NAKs | ❌ Not available |
| **Bitrate Validation** | ✅ Sender vs receiver check | ❌ Not available |
| **Max Error Points** | Higher (RTT+NAK+window penalties) | Lower (bandwidth+loss only) |

## Example Scenarios

### Scenario 1: High RTT Connection

**Keepalive:**
```
[INFO] Per-connection keepalive: ID=0, BW: 2000.00 kbits/s, Window=8192, In-flight=50, RTT=350ms, NAKs=1
```

**Comparison:**
```
[ALGO_CMP] ConnInfo: Err=25 W=70% T=0.70 | Legacy: Err=5 W=85% T=0.85 | Delta: E=+20 W=-15% T=-0.15
```

**Interpretation:**
- Connection Info detects high RTT (350ms > 200ms threshold) → +10 error points
- RTT variance penalty → +10 more error points
- Legacy only sees bandwidth/loss, doesn't detect RTT issue
- **Result**: Connection Info throttles more aggressively (shifts load to better connections)

### Scenario 2: High NAK Rate

**Keepalive:**
```
[INFO] Per-connection keepalive: ID=1, BW: 1500.00 kbits/s, Window=4096, In-flight=2048, RTT=50ms, NAKs=500
```

**Comparison:**
```
[ALGO_CMP] ConnInfo: Err=50 W=10% T=0.20 | Legacy: Err=10 W=70% T=0.70 | Delta: E=+40 W=-60% T=-0.50
```

**Interpretation:**
- High NAK rate (500 NAKs) → +20-40 error points (Connection Info only)
- High window utilization (2048/4096 = 50%) → potential congestion
- Legacy doesn't see sender NAKs, only receiver packet loss
- **Result**: Connection Info severely throttles, Legacy doesn't recognize severity

### Scenario 3: Both Algorithms Agree

**Keepalive:**
```
[INFO] Per-connection keepalive: ID=2, BW: 3000.00 kbits/s, Window=8192, In-flight=100, RTT=30ms, NAKs=2
```

**Comparison:**
```
[DEBUG] [ALGO_CMP] Algorithms agree: Err=0 W=100% (delta: E=+0 W=+0%)
```

**Interpretation:**
- Good bandwidth, low RTT, low NAK rate, good window utilization
- Both algorithms assign 0 error points, 100% weight
- No comparison log at INFO level (reduced spam)

## Analyzing Comparison Data

### Extract Comparison Logs

```bash
# Get all algorithm comparison logs
grep "ALGO_CMP" logs/srtla_rec.log > comparison.log

# Get only divergences (meaningful differences)
grep "ALGO_CMP.*Delta: E=[+-][5-9]" logs/srtla_rec.log
grep "ALGO_CMP.*Delta: E=[+-][0-9][0-9]" logs/srtla_rec.log

# Extract weight deltas
grep -oP 'Delta:.*W=\K[+-][0-9]+' comparison.log
```

### Statistics Script

```bash
#!/bin/bash
# Calculate average deltas

echo "=== Algorithm Comparison Statistics ==="

# Average error delta
grep "ALGO_CMP" logs/srtla_rec.log | \
  grep -oP 'E=\K[+-]?[0-9]+(?= W)' | \
  awk '{sum+=$1; count++} END {print "Avg Error Delta:", sum/count}'

# Average weight delta  
grep "ALGO_CMP" logs/srtla_rec.log | \
  grep -oP 'W=\K[+-]?[0-9]+(?=%)|W=\K[+-]?[0-9]+(?= T)' | \
  awk '{sum+=$1; count++} END {print "Avg Weight Delta:", sum/count "%"}'

# Times Connection Info was more pessimistic
grep "ALGO_CMP" logs/srtla_rec.log | \
  grep -c "E=+[0-9]"
echo "^ Times Connection Info found more errors"

# Times Legacy was more pessimistic
grep "ALGO_CMP" logs/srtla_rec.log | \
  grep -c "E=-[0-9]"
echo "^ Times Legacy found more errors"
```

## Production vs Comparison Mode

### Comparison Mode (ENABLE_ALGO_COMPARISON=1)

**Use when:**
- Developing/testing the connection info algorithm
- Analyzing algorithm behavior differences
- Validating improvements

**Characteristics:**
- Both algorithms run on every evaluation cycle
- Comparison logs when algorithms disagree
- Slightly higher CPU usage (negligible)
- Extra fields in ConnectionStats struct

### Production Mode (ENABLE_ALGO_COMPARISON=0)

**Use when:**
- Deploying to production
- Algorithm is proven and stable
- No need for comparison data

**Characteristics:**
- Only Connection Info algorithm runs
- No comparison logging
- Minimal overhead
- Legacy fields not used

**To switch:**
```cpp
// In src/receiver_config.h
#define ENABLE_ALGO_COMPARISON 0
```

Then rebuild:
```bash
cd build && make -j$(nproc)
```

## Expected Insights

### Connection Info Should Detect:

1. **High RTT**: RTT > 200ms → extra error points
2. **RTT Variance**: Jitter > 50ms → extra error points
3. **High NAK Rate**: Sender NAKs > 10% → extra error points
4. **Window Congestion**: In-flight/window > 95% → extra error points
5. **Bitrate Discrepancies**: Sender vs receiver > 20% → warning logs

### When Algorithms Might Disagree:

- **Connection Info more pessimistic**: Detects latency/congestion issues legacy misses
- **Legacy more pessimistic**: Extremely rare (both use same bandwidth/loss base)
- **Both agree**: Stable, healthy connections with no hidden issues

## Code Locations

| Component | File | Lines |
|-----------|------|-------|
| Comparison flag | `src/receiver_config.h` | 13-15 |
| Legacy algorithm stats | `src/receiver_config.h` | 104-106 |
| Keepalive comparison | `src/protocol/srtla_handler.cpp` | 364-437 |
| Legacy algorithm impl | `src/quality/quality_evaluator.cpp` | 325-373 |
| Quality evaluation | `src/quality/quality_evaluator.cpp` | 182-188 |
| Load balancer comparison | `src/quality/load_balancer.cpp` | 111-127 |

## Notes

- Comparison mode has **minimal performance impact** (both algorithms are lightweight)
- Logs are **non-spammy**: Only shown when algorithms diverge meaningfully (≥5% delta)
- Both algorithms use the **same data** from the same keepalive packets
- The **Connection Info algorithm is active** (makes actual ACK throttling decisions)
- The **Legacy algorithm runs in parallel** for comparison only (results logged but not used)
- Disable comparison mode in production once algorithm is validated

## Disabling Comparison Mode

When you're satisfied with the Connection Info algorithm and don't need comparisons:

1. Edit `src/receiver_config.h`:
   ```cpp
   #define ENABLE_ALGO_COMPARISON 0
   ```

2. Rebuild:
   ```bash
   cd build && make clean && make -j$(nproc)
   ```

3. The legacy algorithm won't run, comparison logs disappear, and you save the extra struct fields.

## Backwards Compatibility: No Connection Info in Keepalive

### What Happens?

If a sender doesn't send connection info in keepalive packets (e.g., older srtla_send clients):

✅ **Both algorithms continue to work**  
✅ **Legacy algorithm**: Unchanged - only needs receiver-side bandwidth and packet loss  
✅ **Connection Info algorithm**: Gracefully degrades to legacy behavior

### Why Both Algorithms Give Same Results Without Connection Info

The Connection Info algorithm **only adds penalties** for:
- RTT variance (requires sender RTT data)
- High sender NAK rate (requires sender NAK count)
- Window congestion (requires sender window/in-flight data)

Without this telemetry, these penalties are all **zero**, making it functionally identical to the legacy algorithm.

### Logs When Connection Info Missing

**Keepalive:**
```
[DEBUG] [IP:PORT] [Group: 0x...] Keepalive without connection info - both algorithms will use receiver-side metrics only
```

**Quality Evaluation (every 5 seconds):**
```
[INFO] [Group: 0x...] Connection parameters adjusted:
[INFO] [IP:PORT] [COMPARISON] ConnInfo: Weight=85%, Throttle=0.85, ErrPts=10 | Legacy: Weight=85%, Throttle=0.85, ErrPts=10 | Delta: W=+0%, T=+0.00, E=+0
```

Notice: **Delta is zero** because both algorithms see the same data and make identical decisions.

### Mixed Environment

If you have **multiple senders** with different capabilities:

- Sender A (new): Sends connection info → Connection Info algorithm uses extra telemetry
- Sender B (old): No connection info → Both algorithms behave identically for this sender

The comparison logs will show:
- Deltas for Sender A's connections (Connection Info finds more issues)
- Zero/minimal deltas for Sender B's connections (both algorithms agree)

This is completely normal and expected!
