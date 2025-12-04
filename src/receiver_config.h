#pragma once

#include <cstdint>

namespace srtla {
inline constexpr int MAX_CONNS_PER_GROUP = 16;
inline constexpr int MAX_GROUPS = 200;

inline constexpr int CLEANUP_PERIOD = 3;
inline constexpr int GROUP_TIMEOUT = 4;
inline constexpr int CONN_TIMEOUT = 4;

inline constexpr int KEEPALIVE_PERIOD = 1;
inline constexpr int RECOVERY_CHANCE_PERIOD = 5;

inline constexpr int CONN_QUALITY_EVAL_PERIOD = 5;
inline constexpr int ACK_THROTTLE_INTERVAL = 100; // milliseconds
inline constexpr double MIN_ACK_RATE = 0.2;
inline constexpr double MIN_ACCEPTABLE_TOTAL_BANDWIDTH_KBPS = 1000.0;
inline constexpr int MAX_ERROR_POINTS = 40;
inline constexpr double GOOD_CONNECTION_THRESHOLD = 0.5;
inline constexpr int CONNECTION_GRACE_PERIOD = 10;

inline constexpr int WEIGHT_FULL = 100;
inline constexpr int WEIGHT_EXCELLENT = 85;
inline constexpr int WEIGHT_DEGRADED = 70;
inline constexpr int WEIGHT_FAIR = 55;
inline constexpr int WEIGHT_POOR = 40;
inline constexpr int WEIGHT_CRITICAL = 10;

inline constexpr std::size_t RECV_ACK_INT = 10;
inline constexpr const char *SRT_SOCKET_INFO_PREFIX = "/tmp/srtla-group-";

struct srtla_ack_pkt {
    uint32_t type;
    uint32_t acks[RECV_ACK_INT];
};

struct ConnectionStats {
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
};

} // namespace srtla
