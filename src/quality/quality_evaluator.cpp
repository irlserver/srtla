#include "quality_evaluator.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <spdlog/spdlog.h>

extern "C" {
#include "../common.h"
}

namespace srtla::quality {

using srtla::connection::ConnectionGroupPtr;
using srtla::connection::ConnectionPtr;

void QualityEvaluator::evaluate_group(ConnectionGroupPtr group, time_t current_time) {
    if (!group || group->connections().empty() || !group->load_balancing_enabled()) {
        return;
    }

    if (group->last_quality_eval() + CONN_QUALITY_EVAL_PERIOD > current_time) {
        return;
    }

    spdlog::debug("[Group: {}] Evaluating connection quality", static_cast<void *>(group.get()));

group->set_total_target_bandwidth(0);
    uint64_t current_ms = 0;
    if (get_ms(&current_ms) != 0) {
        spdlog::error("[Group: {}] Failed to get current timestamp for quality evaluation", 
                      static_cast<void *>(group.get()));
        return;
    }

    std::vector<QualityMetrics> bandwidth_info;
    bandwidth_info.reserve(group->connections().size());

    for (auto &conn : group->connections()) {
        uint64_t time_diff_ms = 0;
        if (conn->stats().last_eval_time > 0) {
            time_diff_ms = current_ms - conn->stats().last_eval_time;
        }

double bandwidth_kbits_per_sec = 0.0;
        double packet_loss_ratio = 0.0;
        
        if (time_diff_ms > 0) {
            uint64_t bytes_diff = conn->stats().bytes_received - conn->stats().last_bytes_received;
            uint64_t packets_diff = conn->stats().packets_received - conn->stats().last_packets_received;
            uint32_t lost_diff = conn->stats().packets_lost - conn->stats().last_packets_lost;

            double seconds = static_cast<double>(time_diff_ms) / 1000.0;
            double bandwidth_bytes_per_sec = bytes_diff / seconds;
            bandwidth_kbits_per_sec = (bandwidth_bytes_per_sec * 8.0) / 1000.0;

            if (packets_diff > 0) {
                packet_loss_ratio = static_cast<double>(lost_diff) / (packets_diff + lost_diff);
            }

            group->set_total_target_bandwidth(group->total_target_bandwidth() + static_cast<uint64_t>(bandwidth_bytes_per_sec));
        }

        bandwidth_info.push_back({bandwidth_kbits_per_sec, packet_loss_ratio, 0});

        conn->stats().last_bytes_received = conn->stats().bytes_received;
        conn->stats().last_packets_received = conn->stats().packets_received;
        conn->stats().last_packets_lost = conn->stats().packets_lost;
        conn->stats().last_eval_time = current_ms;
    }

    if (bandwidth_info.empty()) {
        return;
    }

    double total_kbits_per_sec = (group->total_target_bandwidth() * 8.0) / 1000.0;
    double max_kbits_per_sec = 0.0;
    double median_kbits_per_sec = 0.0;

    std::vector<double> all_bandwidths;
    all_bandwidths.reserve(bandwidth_info.size());
    for (const auto &info : bandwidth_info) {
        all_bandwidths.push_back(info.bandwidth_kbits_per_sec);
        max_kbits_per_sec = std::max(max_kbits_per_sec, info.bandwidth_kbits_per_sec);
    }

    if (!all_bandwidths.empty() && max_kbits_per_sec > 0) {
        double good_threshold = max_kbits_per_sec * GOOD_CONNECTION_THRESHOLD;
        std::vector<double> good_bandwidths;
        for (const auto &bw : all_bandwidths) {
            if (bw >= good_threshold) {
                good_bandwidths.push_back(bw);
            }
        }

        auto compute_median = [](std::vector<double> &values) {
            std::sort(values.begin(), values.end());
            size_t mid = values.size() / 2;
            if (values.size() % 2 == 0) {
                return (values[mid - 1] + values[mid]) / 2.0;
            }
            return values[mid];
        };

        if (!good_bandwidths.empty()) {
            median_kbits_per_sec = compute_median(good_bandwidths);
            spdlog::trace("[Group: {}] Median from good connections (>= {:.2f} kbps): {:.2f} kbps",
                          static_cast<void *>(group.get()), good_threshold, median_kbits_per_sec);
        } else {
            median_kbits_per_sec = compute_median(all_bandwidths);
            spdlog::trace("[Group: {}] Using fallback median from all connections: {:.2f} kbps",
                          static_cast<void *>(group.get()), median_kbits_per_sec);
        }
    }

    double min_expected_kbits_per_sec = std::max(100.0, MIN_ACCEPTABLE_TOTAL_BANDWIDTH_KBPS / bandwidth_info.size());

    spdlog::debug("[Group: {}] Total bandwidth: {:.2f} kbits/s, Max: {:.2f} kbits/s, Median: {:.2f} kbits/s, Min expected per conn: {:.2f} kbps",
                  static_cast<void *>(group.get()), total_kbits_per_sec, max_kbits_per_sec, median_kbits_per_sec,
                  min_expected_kbits_per_sec);

    for (std::size_t idx = 0; idx < bandwidth_info.size() && idx < group->connections().size(); ++idx) {
        auto conn = group->connections()[idx];
        auto &metrics = bandwidth_info[idx];

        bool in_grace_period = (current_time - conn->connection_start()) < CONNECTION_GRACE_PERIOD;
        if (in_grace_period) {
            spdlog::debug("[{}:{}] Connection in grace period, skipping penalties",
                          print_addr(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&conn->address()))),
                          port_no(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&conn->address()))));
            continue;
        }

        conn->stats().error_points = 0;

        bool is_poor_connection = metrics.bandwidth_kbits_per_sec < median_kbits_per_sec * GOOD_CONNECTION_THRESHOLD;
        double expected_kbits_per_sec = is_poor_connection ? min_expected_kbits_per_sec : median_kbits_per_sec;
        expected_kbits_per_sec = std::max(expected_kbits_per_sec, min_expected_kbits_per_sec);

        double performance_ratio = expected_kbits_per_sec > 0 ? metrics.bandwidth_kbits_per_sec / expected_kbits_per_sec : 0;
        
        // ====================================================================
        // CONNECTION INFO ALGORITHM: Uses sender telemetry
        // ====================================================================
        if (performance_ratio < 0.3) {
            conn->stats().error_points += 40;
        } else if (performance_ratio < 0.5) {
            conn->stats().error_points += 25;
        } else if (performance_ratio < 0.7) {
            conn->stats().error_points += 15;
        } else if (performance_ratio < 0.85) {
            conn->stats().error_points += 5;
        }

        if (metrics.packet_loss_ratio > 0.20) {
            conn->stats().error_points += 40;
        } else if (metrics.packet_loss_ratio > 0.10) {
            conn->stats().error_points += 20;
        } else if (metrics.packet_loss_ratio > 0.05) {
            conn->stats().error_points += 10;
        } else if (metrics.packet_loss_ratio > 0.01) {
            conn->stats().error_points += 5;
        }

        // Add RTT-based error points (Phase 1)
        conn->stats().error_points += calculate_rtt_error_points(conn->stats(), current_time);
        
        // Add NAK rate error points (Phase 2)
        uint64_t packets_diff = conn->stats().packets_received - conn->stats().last_packets_received;
        conn->stats().error_points += calculate_nak_error_points(conn->stats(), packets_diff);
        
        // Add window utilization error points (Phase 3)
        conn->stats().error_points += calculate_window_error_points(conn->stats());
        
        // Validate bitrate (Phase 4 - logging only)
        double receiver_bitrate_bps = metrics.bandwidth_kbits_per_sec * 125.0;  // kbits to bytes
        validate_bitrate(conn->stats(), receiver_bitrate_bps, &conn->address());

        conn->stats().nack_count = 0;

#if ENABLE_ALGO_COMPARISON
        // ====================================================================
        // LEGACY ALGORITHM: Parallel evaluation for comparison
        // ====================================================================
        evaluate_connection_legacy(conn, metrics.bandwidth_kbits_per_sec, 
                                   metrics.packet_loss_ratio, performance_ratio, current_time);
#endif

        double log_percentage = 0.0;
        if (is_poor_connection && median_kbits_per_sec > 0) {
            log_percentage = (metrics.bandwidth_kbits_per_sec / median_kbits_per_sec) * 100.0;
        } else if (expected_kbits_per_sec > 0) {
            log_percentage = (metrics.bandwidth_kbits_per_sec / expected_kbits_per_sec) * 100.0;
        }

        spdlog::debug("  [{}:{}] [Group: {}] Connection stats: BW: {:.2f} kbits/s ({:.2f}%), Loss: {:.2f}%, Error points: {}",
                      print_addr(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&conn->address()))),
                      port_no(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&conn->address()))),
                      static_cast<void *>(group.get()),
                      metrics.bandwidth_kbits_per_sec,
                      log_percentage,
                      metrics.packet_loss_ratio * 100.0,
                      conn->stats().error_points);
    }

    group->set_last_quality_eval(current_time);
}

double QualityEvaluator::calculate_rtt_variance(const ConnectionStats &stats) {
    // Count valid samples
    int count = 0;
    double sum = 0;
    for (size_t i = 0; i < RTT_HISTORY_SIZE; i++) {
        if (stats.rtt_history[i] > 0) {
            sum += stats.rtt_history[i];
            count++;
        }
    }
    
    if (count < 2) return 0;  // Need at least 2 samples
    
    double mean = sum / count;
    double variance_sum = 0;
    for (size_t i = 0; i < RTT_HISTORY_SIZE; i++) {
        if (stats.rtt_history[i] > 0) {
            double diff = static_cast<double>(stats.rtt_history[i]) - mean;
            variance_sum += diff * diff;
        }
    }
    
    return std::sqrt(variance_sum / count);
}

uint32_t QualityEvaluator::calculate_rtt_error_points(const ConnectionStats &stats, time_t current_time) {
    // Don't use stale keepalive data
    if (stats.last_keepalive == 0 || (current_time - stats.last_keepalive) > KEEPALIVE_STALENESS_THRESHOLD) {
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

uint32_t QualityEvaluator::calculate_nak_error_points(ConnectionStats &stats, uint64_t packets_diff) {
    if (packets_diff == 0 || stats.sender_nak_count == 0) {
        return 0;
    }
    
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

uint32_t QualityEvaluator::calculate_window_error_points(const ConnectionStats &stats) {
    if (stats.window <= 0) {
        return 0;
    }
    
    double utilization = static_cast<double>(stats.in_flight) / stats.window;
    
    uint32_t points = 0;
    
    // Persistently full window indicates congestion
    if (utilization > WINDOW_UTILIZATION_CONGESTED) {
        points += 15;
    }
    
    // Very low utilization might indicate client-side throttling
    // This is informational, not necessarily bad, so we don't penalize
    
    return points;
}

void QualityEvaluator::validate_bitrate(const ConnectionStats &stats,
                                        double receiver_bitrate_bps,
                                        const struct sockaddr_storage *addr) {
    if (stats.sender_bitrate_bps == 0) {
        return;
    }
    
    double ratio = std::abs(receiver_bitrate_bps - stats.sender_bitrate_bps)
                   / stats.sender_bitrate_bps;
    
    if (ratio > BITRATE_DISCREPANCY_THRESHOLD) {
        spdlog::warn("[{}:{}] Large bitrate discrepancy: "
                     "sender={} bps, receiver={} bps ({:.1f}%)",
                     print_addr(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(addr))),
                     port_no(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(addr))),
                     stats.sender_bitrate_bps,
                     static_cast<uint64_t>(receiver_bitrate_bps),
                     ratio * 100);
    }
}

void QualityEvaluator::evaluate_connection_legacy(ConnectionPtr conn,
                                                   double bandwidth_kbits_per_sec,
                                                   double packet_loss_ratio,
                                                   double performance_ratio,
                                                   time_t current_time) {
    // ========================================================================
    // LEGACY ALGORITHM: No connection info (RTT, window, sender NAKs, etc.)
    // Only uses receiver-side bandwidth and packet loss measurements
    // ========================================================================
    conn->stats().legacy_error_points = 0;
    
    // Bandwidth-based penalties (same as connection info algorithm)
    if (performance_ratio < 0.3) {
        conn->stats().legacy_error_points += 40;
    } else if (performance_ratio < 0.5) {
        conn->stats().legacy_error_points += 25;
    } else if (performance_ratio < 0.7) {
        conn->stats().legacy_error_points += 15;
    } else if (performance_ratio < 0.85) {
        conn->stats().legacy_error_points += 5;
    }
    
    // Packet loss penalties (same as connection info algorithm)
    if (packet_loss_ratio > 0.20) {
        conn->stats().legacy_error_points += 40;
    } else if (packet_loss_ratio > 0.10) {
        conn->stats().legacy_error_points += 20;
    } else if (packet_loss_ratio > 0.05) {
        conn->stats().legacy_error_points += 10;
    } else if (packet_loss_ratio > 0.01) {
        conn->stats().legacy_error_points += 5;
    }
    
    // NOTE: Legacy algorithm does NOT have:
    // - RTT-based penalties
    // - Sender NAK rate analysis
    // - Window utilization penalties
    // - Bitrate discrepancy validation
    
    // Calculate legacy weight and throttle (same logic as connection info)
    if (conn->stats().legacy_error_points >= 40) {
        conn->stats().legacy_weight_percent = WEIGHT_CRITICAL;
    } else if (conn->stats().legacy_error_points >= 30) {
        conn->stats().legacy_weight_percent = WEIGHT_POOR;
    } else if (conn->stats().legacy_error_points >= 20) {
        conn->stats().legacy_weight_percent = WEIGHT_FAIR;
    } else if (conn->stats().legacy_error_points >= 10) {
        conn->stats().legacy_weight_percent = WEIGHT_DEGRADED;
    } else if (conn->stats().legacy_error_points >= 5) {
        conn->stats().legacy_weight_percent = WEIGHT_EXCELLENT;
    } else {
        conn->stats().legacy_weight_percent = WEIGHT_FULL;
    }
    
    conn->stats().legacy_ack_throttle_factor = 
        std::max(MIN_ACK_RATE, static_cast<double>(conn->stats().legacy_weight_percent) / 100.0);
}

} // namespace srtla::quality
