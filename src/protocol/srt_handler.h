#pragma once

#include <sys/epoll.h>

#include "../connection/connection_registry.h"
#include "../metrics/prometheus.h"
#include "../utils/auth_rate_limiter.h"
#include "../utils/network_utils.h"

namespace srtla::protocol {

class SRTHandler {
public:
    SRTHandler(int srtla_socket,
               const struct sockaddr_storage &srt_addr,
               int epoll_fd,
               connection::ConnectionRegistry &registry,
               utils::AuthRateLimiter &rate_limiter);

    void handle_srt_data(connection::ConnectionGroupPtr group);
    // Returns false only when the group was torn down. A packet dropped
    // because the SRT socket's send buffer was full returns true.
    bool forward_to_srt_server(connection::ConnectionGroupPtr group, const char *buffer, int length);

private:
    bool ensure_group_socket(connection::ConnectionGroupPtr group);
    // reason labels the srtla_groups_removed_total counter for this teardown.
    void remove_group(connection::ConnectionGroupPtr group,
                      metrics::Counter reason = metrics::GROUPS_REMOVED_SRT_ERROR);

    int srtla_socket_;
    struct sockaddr_storage srt_addr_ {};
    int epoll_fd_;
    connection::ConnectionRegistry &registry_;
    utils::AuthRateLimiter &rate_limiter_;
};

} // namespace srtla::protocol
