#include "metrics_collector.h"

namespace srtla::quality {

void MetricsCollector::on_packet_received(connection::ConnectionPtr conn, size_t bytes) {
    auto &stats = conn->stats();
    stats.bytes_received += bytes;
    stats.packets_received++;
}

void MetricsCollector::on_nak_detected(connection::ConnectionPtr conn, uint32_t nak_count) {
    auto &stats = conn->stats();
    stats.packets_lost += nak_count;
    stats.nack_count += nak_count;
}

void MetricsCollector::reset_period(connection::ConnectionPtr conn, uint64_t current_ms) {
    auto &stats = conn->stats();
    stats.last_bytes_received = stats.bytes_received;
    stats.last_packets_received = stats.packets_received;
    stats.last_packets_lost = stats.packets_lost;
    stats.last_eval_time = current_ms;
}

uint64_t MetricsCollector::bytes_in_period(const connection::ConnectionPtr &conn) const {
    const auto &stats = conn->stats();
    return stats.bytes_received - stats.last_bytes_received;
}

uint64_t MetricsCollector::packets_in_period(const connection::ConnectionPtr &conn) const {
    const auto &stats = conn->stats();
    return stats.packets_received - stats.last_packets_received;
}

uint32_t MetricsCollector::naks_in_period(const connection::ConnectionPtr &conn) const {
    const auto &stats = conn->stats();
    return stats.packets_lost - stats.last_packets_lost;
}

} // namespace srtla::quality
