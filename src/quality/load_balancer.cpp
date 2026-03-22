#include "load_balancer.h"

#include <algorithm>

#include <spdlog/spdlog.h>

extern "C" {
#include "../common.h"
}

#include "../receiver_config.h"

namespace srtla::quality {

using srtla::connection::ConnectionGroupPtr;

void LoadBalancer::adjust_weights(ConnectionGroupPtr group, time_t current_time) const {
    if (!group || group->connections().empty()) {
        return;
    }

    const bool load_balancing_enabled = group->load_balancing_enabled();

    if (load_balancing_enabled) {
        if (group->last_load_balance_eval() >= group->last_quality_eval()) {
            return;
        }
    } else {
        time_t last_eval = group->last_load_balance_eval();
        if (last_eval != 0 && (last_eval + CONN_QUALITY_EVAL_PERIOD) > current_time) {
            return;
        }
    }

    group->set_last_load_balance_eval(current_time);

    bool any_change = false;
    spdlog::debug("[Group: {}] Evaluating weights for {} connections",
                  static_cast<void *>(group.get()), group->connections().size());


    uint8_t max_weight = 0;
    int active_conns = 0;

    for (auto &conn : group->connections()) {
        uint8_t old_weight = conn->stats().weight_percent;
        uint8_t new_weight;

        if (conn->stats().error_points >= 40) {
            new_weight = WEIGHT_CRITICAL;
        } else if (conn->stats().error_points >= 25) {
            new_weight = WEIGHT_POOR;
        } else if (conn->stats().error_points >= 15) {
            new_weight = WEIGHT_FAIR;
        } else if (conn->stats().error_points >= 10) {
            new_weight = WEIGHT_DEGRADED;
        } else if (conn->stats().error_points >= 5) {
            new_weight = WEIGHT_EXCELLENT;
        } else {
            new_weight = WEIGHT_FULL;
        }

        if (new_weight != old_weight) {
            conn->stats().weight_percent = new_weight;
            any_change = true;
        }

        if (!((conn->last_received() + CONN_TIMEOUT) < current_time)) {
            max_weight = std::max(max_weight, conn->stats().weight_percent);
            active_conns++;
        }
    }

    spdlog::debug("[Group: {}] Active connections: {}, max_weight: {}, load_balancing_enabled: {}",
                  static_cast<void *>(group.get()), active_conns, max_weight, load_balancing_enabled);
 
    // Note: ACK throttling has been removed. SRTLA ACKs are now sent
    // unconditionally every RECV_ACK_INT packets, matching the behavior
    // of other SRTLA implementations. Weight assignments above are kept
    // for quality assessment and logging purposes.

    if (any_change) {
        spdlog::info("[Group: {}] Connection parameters adjusted:", static_cast<void *>(group.get()));
        for (auto &conn : group->connections()) {
#if ENABLE_ALGO_COMPARISON
            // Show side-by-side comparison of both algorithms
            int error_delta = static_cast<int>(conn->stats().error_points) - static_cast<int>(conn->stats().legacy_error_points);
            int weight_delta = static_cast<int>(conn->stats().weight_percent) - static_cast<int>(conn->stats().legacy_weight_percent);

            spdlog::info("  [{}:{}] [COMPARISON] ConnInfo: Weight={}%, ErrPts={} | Legacy: Weight={}%, ErrPts={} | Delta: W={:+d}%, E={:+d}",
                         print_addr(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&conn->address()))),
                         port_no(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&conn->address()))),
                         conn->stats().weight_percent,
                         conn->stats().error_points,
                         conn->stats().legacy_weight_percent,
                         conn->stats().legacy_error_points,
                         weight_delta,
                         error_delta);
#else
            spdlog::info("  [{}:{}] Weight: {}%, Error points: {}, Bandwidth: {} bytes, Packets: {}, Loss: {}",
                         print_addr(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&conn->address()))),
                         port_no(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&conn->address()))),
                         conn->stats().weight_percent,
                         conn->stats().error_points,
                         conn->stats().bytes_received,
                         conn->stats().packets_received,
                         conn->stats().packets_lost);
#endif
        }
    } else {
        spdlog::debug("[Group: {}] No weight adjustments needed", static_cast<void *>(group.get()));
    }
}

} // namespace srtla::quality
