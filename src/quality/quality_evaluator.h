#pragma once

#include "metrics_collector.h"
#include "../connection/connection_group.h"

namespace srtla::quality {

struct QualityMetrics {
    double bandwidth_kbits_per_sec = 0.0;
    double packet_loss_ratio = 0.0;
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
};

} // namespace srtla::quality
