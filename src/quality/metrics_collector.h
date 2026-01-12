#pragma once

#include "../receiver_config.h"
#include "../connection/connection.h"

namespace srtla::quality {

class MetricsCollector {
public:
    void on_packet_received(connection::ConnectionPtr conn, size_t bytes);
    void on_nak_detected(connection::ConnectionPtr conn, uint32_t nak_count);

    void reset_period(connection::ConnectionPtr conn, uint64_t current_ms);

    uint64_t bytes_in_period(const connection::ConnectionPtr &conn) const;
    uint64_t packets_in_period(const connection::ConnectionPtr &conn) const;
    uint32_t naks_in_period(const connection::ConnectionPtr &conn) const;
};

} // namespace srtla::quality
