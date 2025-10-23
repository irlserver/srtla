# SRTLA Receiver (srtla_rec)

## Overview

srtla_rec is an SRT transport proxy with link aggregation. SRTLA is designed to transport [SRT](https://github.com/Haivision/srt/) traffic over multiple network links for capacity aggregation and redundancy. Traffic is balanced dynamically depending on network conditions. The primary application is bonding mobile modems for live streaming.

> **Note**: This is a fork of the original SRTLA implementation by BELABOX. The original server component (srtla_rec) was marked as unsupported by BELABOX.

## Features

- Support for link aggregation across multiple network connections
- Automatic management of connection groups and individual connections
- Robust error handling and timeouts for inactive connections
- Logging of connection details for easy diagnostics
- Improved load balancing through ACK throttling
- Connection recovery mechanism for temporary network issues

## Requirements

- C++11 compatible compiler
- CMake for the build process
- spdlog library
- argparse library

## Assumptions and Prerequisites

SRTLA assumes that:

- Data is streamed from an SRT _sender_ in _caller_ mode to an SRT _receiver_ in _listener_ mode
- To benefit from link aggregation, the _sender_ should have 2 or more network links to the SRT listener (typically internet-connected modems)
- The sender needs to have source routing configured, as SRTLA uses `bind()` to map UDP sockets to specific connections

## Installation

```bash
# Clone the repository
git clone https://github.com/OpenIRL/srtla.git
cd srtla

# Build with CMake
mkdir build
cd build
cmake ..
make
```

## Usage

srtla_rec runs as a proxy between SRTla clients and an SRT server:

```bash
./srtla_rec [OPTIONS]
```

### Command Line Options

- `--srtla_port PORT`: Port to bind the SRTLA socket to (default: 5000)
- `--srt_hostname HOST`: Hostname of the downstream SRT server (default: 127.0.0.1)
- `--srt_port PORT`: Port of the downstream SRT server (default: 4001)
- `--verbose`: Enable verbose logging (default: disabled)
- `--debug`: Enable debug logging (default: disabled)

### Example

```bash
./srtla_rec --srtla_port 5000 --srt_hostname 192.168.1.10 --srt_port 4001 --verbose
```

## How It Works

1. srtla_rec creates a UDP socket for incoming SRTLA connections.
2. Clients register with srtla_rec and create connection groups.
3. Multiple connections can be added to a group.
4. Data is received across all connections and forwarded to the SRT server.
5. ACK packets are sent across all connections for timely delivery.
6. Inactive connections and groups are automatically cleaned up.

### Technical Details

SRTLA implements a protocol for packet transmission over multiple network connections, aggregating the data and making it available to the SRT protocol. The implementation is based on the following core mechanisms:

1. **Connection Group Management**: The software organizes connections into groups, with each group corresponding to an SRT stream. This enables support for multiple simultaneous SRTLA senders with a single receiver.

2. **Packet Tracking**: The code tracks received packets with sequence numbers and periodically sends SRTLA-ACK packets back to confirm receipt.

3. **Two-phase Registration Process**:

   - Sender (conn 0): `SRTLA_REG1` (contains sender-generated random ID)
   - Receiver: `SRTLA_REG2` (contains full ID with receiver-generated values)
   - Sender (conn 0): `SRTLA_REG2` (with full ID)
   - Receiver: `SRTLA_REG3`
   - Additional connections follow a similar pattern

4. **Error Handling**: The receiver can send error responses:

   - `SRTLA_REG_ERR`: Operation temporarily failed
   - `SRTLA_REG_NGP`: Invalid ID, group must be re-registered

5. **Connection Cleanup**: Inactive connections and groups are automatically cleaned up after a configurable timeout (default: 10 seconds).

6. **Load Balancing through ACK Throttling**: The server controls ACK frequency to influence the client's connection selection without requiring client-side modifications.

7. **Connection Recovery Mechanism**: Connections that show signs of recovery after temporary outages are given a chance to stabilize again.

The implementation uses epoll for event-based network I/O, allowing efficient handling of multiple simultaneous connections.

## Enhanced Load Balancing and Recovery

This version of SRTLA includes improvements to address two key issues in the original implementation:

### Problem 1: Connections with Issues Had No Recovery Path

In the original implementation, connections with temporary problems were completely disabled. In this enhanced version:

- Connections showing signs of recovery enter a "recovery mode"
- These connections receive more frequent keepalive packets for a set period (5 seconds)
- After successful recovery, they are fully reactivated for data transmission
- Recovery attempts are abandoned after a certain time if unsuccessful

This functionality allows connections to "heal" after brief disruptions (e.g., due to network issues) rather than remaining completely disabled.

### Problem 2: Unbalanced Connection Utilization

In the original implementation, load was unevenly distributed across available connections. The new implementation:

- Introduces a monitoring and evaluation system for connection quality
- Checks connection quality every 5 seconds based on:
  - Bandwidth (kbits/s) and performance ratio (actual vs expected bandwidth)
  - Packet loss rate
  - Connection performance relative to median bandwidth
- Assigns error points to each connection based on these metrics
- Calculates a quality weight for each connection (10% to 100%)
- Controls ACK packet frequency based on connection quality
  - Good connections receive ACKs more frequently
  - Poor connections receive ACKs less frequently
- Indirectly influences the window size in the client and thus connection selection

The result is better data distribution, with more stable connections carrying more load than problematic ones, without requiring client modifications.

### Technical Implementation Details

#### ACK Throttling

The central innovation of this solution is ACK throttling for load distribution. It's based on the following principles:

1. The SRT/SRTLA client (srtla_send) selects connections based on a score derived from the window size and in-flight packets.
2. The window size in the client is adjusted when ACKs are received.
3. By selectively throttling ACK frequency, we can indirectly control how quickly the window grows in the client.
4. This causes the client to prefer better connections without requiring changes to the client code.

#### Connection Quality Assessment

Connection quality is assessed by measuring and analyzing:

- **Bandwidth Performance**: The system calculates a performance ratio by comparing actual bandwidth to expected bandwidth. Poor performance relative to expectations leads to more error points
- **Packet Loss**: Higher loss rates lead to more error points
- **Dynamic Bandwidth Evaluation**: Connections are evaluated against either median bandwidth (for good connections) or minimum threshold (for poor connections)
- **Grace Period**: New connections receive a 10-second grace period before penalties are applied

The weight levels are:

- 100% (WEIGHT_FULL): Optimal connection
- 85% (WEIGHT_EXCELLENT): Excellent connection
- 70% (WEIGHT_DEGRADED): Slightly impaired connection
- 55% (WEIGHT_FAIR): Fair connection
- 40% (WEIGHT_POOR): Severely impaired connection
- 10% (WEIGHT_CRITICAL): Critically impaired connection

#### Recovery Mechanism

The recovery functionality works as follows:

1. A connection that receives data again after being marked inactive is placed in recovery mode
2. In this mode, keepalive packets are sent more frequently (every 1 second)
3. If the connection remains stable for a short period (5 seconds), it is considered recovered
4. If recovery does not occur within the time window, the recovery attempt is aborted

### Configuration Parameters

The following parameters can be adjusted to optimize behavior:

- `KEEPALIVE_PERIOD`: Interval for keepalive packets during recovery (1 second)
- `RECOVERY_CHANCE_PERIOD`: Period during which a connection can attempt to recover (5 seconds)
- `CONN_QUALITY_EVAL_PERIOD`: Interval for evaluating connection quality (5 seconds)
- `ACK_THROTTLE_INTERVAL`: Base interval for ACK throttling (100ms)
- `MIN_ACK_RATE`: Minimum ACK rate to keep connections alive (20%)
- `MIN_ACCEPTABLE_TOTAL_BANDWIDTH_KBPS`: Minimum total bandwidth for acceptable streaming quality (1000 kbps)
- `GOOD_CONNECTION_THRESHOLD`: Threshold for considering a connection "good" (50% of max bandwidth)
- `CONNECTION_GRACE_PERIOD`: Grace period in seconds before applying penalties (10 seconds)
- Various weight levels (`WEIGHT_FULL`, `WEIGHT_EXCELLENT`, `WEIGHT_DEGRADED`, `WEIGHT_FAIR`, `WEIGHT_POOR`, `WEIGHT_CRITICAL`)

### Limitations

- The error point thresholds are static and could be dynamically adjusted to better adapt to different network situations
- The throttling might be less effective with very short ACK intervals
- Performance ratio calculations are based on bandwidth expectations that may need tuning for different network environments

## SRT Configuration Recommendations

The sender should implement congestion control using adaptive bitrate based on the SRT `SRTO_SNDDATA` size or measured RTT.

## Socket Information

srtla_rec creates information files about active connections under `/tmp/srtla-group-[PORT]`. These files contain the client IP addresses connected to a specific socket.

## License

This project is licensed under the [GNU Affero General Public License v3.0](LICENSE):

- Copyright (C) 2020-2021 BELABOX project
- Copyright (C) 2024 IRLToolkit Inc.
- Copyright (C) 2024 OpenIRL
- Copyright (C) 2025 IRLServer.com

You can use, modify, and distribute this code according to the terms of the AGPL-3.0.
