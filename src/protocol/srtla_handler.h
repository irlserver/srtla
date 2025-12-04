#pragma once

#include "srt_handler.h"
#include "../connection/connection_registry.h"
#include "../quality/metrics_collector.h"
#include "../utils/nak_dedup.h"

namespace srtla::protocol {

class SRTLAHandler {
public:
    SRTLAHandler(int srtla_socket,
                 connection::ConnectionRegistry &registry,
                 SRTHandler &srt_handler,
                 quality::MetricsCollector &metrics_collector);

    void process_packet(time_t ts);
    void send_keepalive(const connection::ConnectionPtr &conn, time_t ts);

private:
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

    int srtla_socket_;
    connection::ConnectionRegistry &registry_;
    SRTHandler &srt_handler_;
    quality::MetricsCollector &metrics_;
};

} // namespace srtla::protocol
