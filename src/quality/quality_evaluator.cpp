#include "quality_evaluator.h"

#include <algorithm>
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
    get_ms(&current_ms);

    std::vector<QualityMetrics> bandwidth_info;
    bandwidth_info.reserve(group->connections().size());

    for (auto &conn : group->connections()) {
        uint64_t time_diff_ms = 0;
        if (conn->stats().last_eval_time > 0) {
            time_diff_ms = current_ms - conn->stats().last_eval_time;
        }

        if (time_diff_ms > 0) {
            uint64_t bytes_diff = conn->stats().bytes_received - conn->stats().last_bytes_received;
            uint64_t packets_diff = conn->stats().packets_received - conn->stats().last_packets_received;
            uint32_t lost_diff = conn->stats().packets_lost - conn->stats().last_packets_lost;

            double seconds = static_cast<double>(time_diff_ms) / 1000.0;
            double bandwidth_bytes_per_sec = bytes_diff / seconds;
            double bandwidth_kbits_per_sec = (bandwidth_bytes_per_sec * 8.0) / 1000.0;

            double packet_loss_ratio = 0.0;
            if (packets_diff > 0) {
                packet_loss_ratio = static_cast<double>(lost_diff) / (packets_diff + lost_diff);
            }

            bandwidth_info.push_back({bandwidth_kbits_per_sec, packet_loss_ratio, 0});
            group->set_total_target_bandwidth(group->total_target_bandwidth() + static_cast<uint64_t>(bandwidth_bytes_per_sec));
        }

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

        conn->stats().nack_count = 0;

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

} // namespace srtla::quality
