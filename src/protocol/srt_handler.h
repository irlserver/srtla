#pragma once

#include <sys/epoll.h>

#include "../connection/connection_registry.h"
#include "../utils/network_utils.h"

namespace srtla::protocol {

class SRTHandler {
public:
    SRTHandler(int srtla_socket,
               const struct sockaddr_storage &srt_addr,
               int epoll_fd,
               connection::ConnectionRegistry &registry);

    void handle_srt_data(connection::ConnectionGroupPtr group);
    bool forward_to_srt_server(connection::ConnectionGroupPtr group, const char *buffer, int length);

private:
    bool ensure_group_socket(connection::ConnectionGroupPtr group);
    void remove_group(connection::ConnectionGroupPtr group);

    int srtla_socket_;
    struct sockaddr_storage srt_addr_ {};
    int epoll_fd_;
    connection::ConnectionRegistry &registry_;
};

} // namespace srtla::protocol
