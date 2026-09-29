#pragma once

#include <cstdint>
#include <string>

#include "../connection/connection_registry.h"
#include "../utils/auth_rate_limiter.h"

namespace srtla::metrics {

// Counters are plain integers, not atomics: the receiver is a single epoll loop
// on one thread and the scrape is served from that same loop.
enum Counter {
    PACKETS_RECEIVED,
    BYTES_RECEIVED,
    RECV_BATCHES,
    FORWARDED_PACKETS,
    FORWARDED_BYTES,
    FORWARD_ERRORS,
    FORWARD_DROPPED,
    SRT_PACKETS_RECEIVED,
    SRT_BYTES_RECEIVED,
    NAKS_RECEIVED,
    NAKS_SUPPRESSED,
    GROUP_REGISTRATIONS,
    GROUP_REG_THROTTLED,
    GROUP_REG_TABLE_FULL,
    GROUP_REG_DUP_ADDR,
    GROUP_REG_SEND_ERROR,
    CONN_REGISTRATIONS,
    CONN_REG_NO_GROUP,
    CONN_REG_GROUP_MISMATCH,
    CONN_REG_MAX_CONNS,
    CONN_REG_SEND_ERROR,
    GROUPS_REMOVED_IDLE,
    GROUPS_REMOVED_AUTH,
    GROUPS_REMOVED_SRT_ERROR,
    GROUPS_REMOVED_EVICTED,
    CONNECTIONS_REMOVED,
    AUTH_FAILURES,
    SEND_ERR_ACK,
    SEND_ERR_KEEPALIVE,
    SEND_ERR_DOWNSTREAM,
    RECOVERY_STARTED,
    RECOVERY_COMPLETED,
    RECOVERY_FAILED,
    COUNTER_COUNT
};

void inc(Counter c, uint64_t n = 1);
uint64_t value(Counter c);

class Exporter {
public:
    Exporter(connection::ConnectionRegistry &registry, utils::AuthRateLimiter &rate_limiter);
    ~Exporter();

    // Binds the scrape listener and hands it to the main epoll loop. Port 0
    // disables the exporter entirely. bind_addr must be numeric.
    bool start(const std::string &bind_addr, uint16_t port, int epoll_fd, bool detailed);

    // Serves every scrape pending on the listener. Called when the main loop
    // sees an event tagged with this object.
    void handle_event();

    // Prometheus text exposition of the counters above plus gauges read live
    // from the registry.
    std::string render(time_t now) const;

private:
    connection::ConnectionRegistry &registry_;
    utils::AuthRateLimiter &rate_limiter_;
    int listen_fd_ = -1;
    bool detailed_ = false;
    time_t start_time_ = 0;
};

} // namespace srtla::metrics
