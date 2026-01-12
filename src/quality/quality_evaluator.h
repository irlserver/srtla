#pragma once

#include <cstdint>

#include "metrics_collector.h"
#include "../connection/connection_group.h"

namespace srtla::quality {

struct QualityMetrics {
    double bandwidth_kbits_per_sec = 0.0;
    double packet_loss_ratio = 0.0;
    uint64_t packets_diff = 0;
    uint32_t error_points = 0;
};

class QualityEvaluator {
public:
    QualityEvaluator() = default;

    void evaluate_group(connection::ConnectionGroupPtr group,
                        time_t current_time);

private:
    void evaluate_connection(connection::ConnectionGroupPtr group,
                              const connection::ConnectionPtr &conn,
                              double bandwidth_kbits_per_sec,
                              double packet_loss_ratio,
                              double median_kbits_per_sec,
                              double min_expected_kbits_per_sec,
                              bool is_poor_connection);
    
    // Helper functions for RTT-based quality assessment (Connection Info algorithm)
    uint32_t calculate_rtt_error_points(const ConnectionStats &stats, time_t current_time);
    double calculate_rtt_variance(const ConnectionStats &stats);
    
    // Helper functions for NAK rate analysis (Connection Info algorithm)
    uint32_t calculate_nak_error_points(ConnectionStats &stats, uint64_t packets_diff);
    
    // Helper functions for window utilization (Connection Info algorithm)
    uint32_t calculate_window_error_points(const ConnectionStats &stats);
    
    // Helper function for bitrate validation (Connection Info algorithm)
    void validate_bitrate(const ConnectionStats &stats,
                         double receiver_bitrate_bps,
                         const struct sockaddr_storage *addr);
    
    // Legacy algorithm (without connection info)
    void evaluate_connection_legacy(connection::ConnectionPtr conn,
                                    double bandwidth_kbits_per_sec,
                                    double packet_loss_ratio,
                                    double performance_ratio,
                                    time_t current_time);
};

} // namespace srtla::quality
