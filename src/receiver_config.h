#pragma once

#include <cstdint>
#include <ctime>

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

// RTT-based quality assessment thresholds (microseconds)
inline constexpr uint64_t RTT_THRESHOLD_CRITICAL = 500000; // 500ms
inline constexpr uint64_t RTT_THRESHOLD_HIGH = 200000;     // 200ms
inline constexpr uint64_t RTT_THRESHOLD_MODERATE = 100000; // 100ms
inline constexpr uint64_t RTT_VARIANCE_THRESHOLD = 50000;  // 50ms stddev
inline constexpr int KEEPALIVE_STALENESS_THRESHOLD = 2;    // seconds
inline constexpr std::size_t RTT_HISTORY_SIZE = 5;

// NAK rate thresholds
inline constexpr double NAK_RATE_CRITICAL = 0.20; // 20%
inline constexpr double NAK_RATE_HIGH = 0.10;     // 10%
inline constexpr double NAK_RATE_MODERATE = 0.05; // 5%
inline constexpr double NAK_RATE_LOW = 0.01;      // 1%

// Window utilization thresholds
inline constexpr double WINDOW_UTILIZATION_CONGESTED = 0.95;
inline constexpr double WINDOW_UTILIZATION_LOW = 0.30;

// Bitrate comparison tolerance
inline constexpr double BITRATE_DISCREPANCY_THRESHOLD = 0.20; // 20%

inline constexpr std::size_t RECV_ACK_INT = 10;
inline constexpr const char *SRT_SOCKET_INFO_PREFIX = "/tmp/srtla-group-";

struct srtla_ack_pkt {
  uint32_t type;
  uint32_t acks[RECV_ACK_INT];
};

struct ConnectionStats {
  // Receiver-side metrics
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

  // Sender-side telemetry from keepalive packets
  uint64_t rtt_us = 0;
  uint64_t rtt_history[RTT_HISTORY_SIZE] = {0};
  uint8_t rtt_history_idx = 0;
  time_t last_keepalive = 0;

  int32_t window = 0;
  int32_t in_flight = 0;

  uint32_t sender_nak_count = 0;
  uint32_t last_sender_nak_count = 0;

  uint32_t sender_bitrate_bps = 0;
};

} // namespace srtla
