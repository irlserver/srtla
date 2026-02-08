#pragma once

#include "srt_handler.h"
#include "../connection/connection_registry.h"
#include "../quality/metrics_collector.h"
#include "../utils/nak_dedup.h"

namespace srtla::protocol {

// Batch receive configuration
inline constexpr int RECV_BATCH_SIZE = 64;

class SRTLAHandler {
public:
    SRTLAHandler(int srtla_socket,
                 connection::ConnectionRegistry &registry,
                 SRTHandler &srt_handler,
                 quality::MetricsCollector &metrics_collector);

    // Process multiple packets in a batch using recvmmsg
    int process_packets(time_t ts);
    void send_keepalive(const connection::ConnectionPtr &conn, time_t ts);

private:
    // Process a single packet from the batch
    void process_single_packet(const char *buf, int len,
                               const struct sockaddr_storage *addr, time_t ts);

    int register_group(const struct sockaddr_storage *addr, const char *buffer, time_t ts);
    int register_connection(const struct sockaddr_storage *addr, const char *buffer, time_t ts);
    void register_packet(connection::ConnectionGroupPtr group,
                         const connection::ConnectionPtr &conn,
                         int32_t sn);

    void handle_keepalive(connection::ConnectionGroupPtr group,
                          const connection::ConnectionPtr &conn,
                          const struct sockaddr_storage *addr,
                          const char *buffer,
                          int length);

    // Helper functions for keepalive telemetry
    void update_rtt_history(ConnectionStats &stats, uint32_t rtt);
    void update_connection_telemetry(const connection::ConnectionPtr &conn,
                                     const connection_info_t &info,
                                     time_t current_time);

    int srtla_socket_;
    connection::ConnectionRegistry &registry_;
    SRTHandler &srt_handler_;
    quality::MetricsCollector &metrics_;
};

} // namespace srtla::protocol
