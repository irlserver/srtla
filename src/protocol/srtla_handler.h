#pragma once

#include "srt_handler.h"
#include "../connection/connection_registry.h"
#include "../quality/metrics_collector.h"
#include "../utils/nak_dedup.h"
#include "../security/rate_limiter.h"
#include "../security/stream_id_validator.h"

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

    // Anti-DoS: Set the StreamID validator (optional — nullptr disables validation)
    void set_stream_id_validator(security::StreamIdValidator *validator) {
        stream_id_validator_ = validator;
    }

    // Anti-DoS: Get the rate limiter for periodic cleanup
    security::RateLimiter &rate_limiter() { return rate_limiter_; }

    // Graceful shutdown: notify all connected senders before exit
    void notify_shutdown();

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

    // Anti-DoS: Attempt StreamID validation on an unauthenticated group
    void try_authenticate_group(connection::ConnectionGroupPtr group,
                                const char *buf, int len);

    // Anti-DoS: Count unauthenticated groups in registry
    std::size_t count_pending_groups() const;

    int srtla_socket_;
    connection::ConnectionRegistry &registry_;
    SRTHandler &srt_handler_;
    quality::MetricsCollector &metrics_;

    // Anti-DoS components
    security::RateLimiter rate_limiter_;
    security::StreamIdValidator *stream_id_validator_ = nullptr;
};

} // namespace srtla::protocol

