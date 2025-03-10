# SRTLA Receiver (srtla_rec)

## Overview

srtla_rec is an SRT transport proxy with link aggregation. SRTLA is designed to transport [SRT](https://github.com/Haivision/srt/) traffic over multiple network links for capacity aggregation and redundancy. Traffic is balanced dynamically depending on network conditions. The primary application is bonding mobile modems for live streaming.

> **Note**: This is a fork of the original SRTLA implementation by BELABOX. The original server component (srtla_rec) was marked as unsupported by BELABOX.

## Features

- Support for link aggregation across multiple network connections
- Automatic management of connection groups and individual connections
- Robust error handling and timeouts for inactive connections
- Logging of connection details for easy diagnostics

## Requirements

- C++11 compatible compiler
- CMake for the build process
- spdlog library
- argparse library

## Assumptions and Prerequisites

SRTLA assumes that:
- Data is streamed from an SRT *sender* in *caller* mode to an SRT *receiver* in *listener* mode
- To benefit from link aggregation, the *sender* should have 2 or more network links to the SRT listener (typically internet-connected modems)
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

The implementation uses epoll for event-based network I/O, allowing efficient handling of multiple simultaneous connections.

## SRT Configuration Recommendations

The sender should implement congestion control using adaptive bitrate based on the SRT `SRTO_SNDDATA` size or measured RTT.

## Socket Information

srtla_rec creates information files about active connections under `/tmp/srtla-group-[PORT]`. These files contain the client IP addresses connected to a specific socket.

## License

This project is licensed under the [GNU Affero General Public License v3.0](LICENSE):

- Copyright (C) 2020-2021 BELABOX project
- Copyright (C) 2024 IRLToolkit Inc.
- Copyright (C) 2024 OpenIRL

You can use, modify, and distribute this code according to the terms of the AGPL-3.0.