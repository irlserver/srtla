#include "srt_handler.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

extern "C" {
#include "../common.h"
}

namespace srtla::protocol {

SRTHandler::SRTHandler(int srtla_socket,
                       const struct sockaddr_storage &srt_addr,
                       int epoll_fd,
                       connection::ConnectionRegistry &registry)
    : srtla_socket_(srtla_socket), srt_addr_(srt_addr), epoll_fd_(epoll_fd), registry_(registry) {}

void SRTHandler::handle_srt_data(connection::ConnectionGroupPtr group) {
    if (!group) {
        return;
    }

    char buf[MTU];
    int n = recv(group->srt_socket(), buf, MTU, 0);
    if (n < SRT_MIN_LEN) {
        spdlog::error("[Group: {}] Failed to read the SRT sock, terminating the group",
                      static_cast<void *>(group.get()));
        remove_group(group);
        return;
    }

    if (is_srt_ack(buf, n)) {
        for (auto &conn : group->connections()) {
            int ret = sendto(srtla_socket_, &buf, n, 0,
                             reinterpret_cast<const struct sockaddr *>(&conn->address()), sizeof(struct sockaddr_storage));
            if (ret != n) {
                spdlog::error("[{}:{}] [Group: {}] Failed to send the SRT ack",
                              print_addr(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&conn->address()))),
                              port_no(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&conn->address()))),
                              static_cast<void *>(group.get()));
            }
        }
    } else {
        int ret = sendto(srtla_socket_, &buf, n, 0,
                         reinterpret_cast<const struct sockaddr *>(&group->last_address()), sizeof(struct sockaddr_storage));
        if (ret != n) {
            spdlog::error("[{}:{}] [Group: {}] Failed to send the SRT packet",
                          print_addr(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&group->last_address()))),
                          port_no(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&group->last_address()))),
                          static_cast<void *>(group.get()));
        }
    }
}

bool SRTHandler::forward_to_srt_server(connection::ConnectionGroupPtr group, const char *buffer, int length) {
    if (!ensure_group_socket(group)) {
        return false;
    }

    int ret = send(group->srt_socket(), buffer, length, 0);
    if (ret != length) {
        spdlog::error("[Group: {}] Failed to forward SRTLA packet, terminating the group",
                      static_cast<void *>(group.get()));
        remove_group(group);
        return false;
    }
    return true;
}

bool SRTHandler::ensure_group_socket(connection::ConnectionGroupPtr group) {
    if (group->srt_socket() >= 0) {
        return true;
    }

    int sock = socket(srt_addr_.ss_family, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (sock < 0) {
        spdlog::error("[Group: {}] Failed to create an SRT socket", static_cast<void *>(group.get()));
        remove_group(group);
        return false;
    }

    int bufsize = RECV_BUF_SIZE;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize)) != 0) {
        spdlog::error("failed to set receive buffer size ({})", bufsize);
        close(sock);
        remove_group(group);
        return false;
    }

    int sndbufsize = SEND_BUF_SIZE;
    if (setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbufsize, sizeof(sndbufsize)) != 0) {
        spdlog::error("failed to set send buffer size ({})", sndbufsize);
        close(sock);
        remove_group(group);
        return false;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags == -1 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) == -1) {
        spdlog::error("failed to set g->srt_sock non-blocking");
        close(sock);
        remove_group(group);
        return false;
    }

    int ret = -1;
    if (srt_addr_.ss_family == AF_INET) {
        ret = connect(sock, reinterpret_cast<const struct sockaddr *>(&srt_addr_), sizeof(struct sockaddr_in));
    } else if (srt_addr_.ss_family == AF_INET6) {
        ret = connect(sock, reinterpret_cast<const struct sockaddr *>(&srt_addr_), sizeof(struct sockaddr_in6));
    }

    if (ret != 0) {
        
        spdlog::error("[Group: {}] Failed to connect to SRT server: {}", static_cast<void *>(group.get()), strerror(errno));
        close(sock);
        remove_group(group);
        return false;
    }

    uint16_t local_port = utils::NetworkUtils::get_local_port(sock);
    spdlog::info("[Group: {}] Created SRT socket. Local Port: {}", static_cast<void *>(group.get()), local_port);

    if (utils::NetworkUtils::epoll_add(epoll_fd_, sock, EPOLLIN, group.get()) != 0) {
        spdlog::error("[Group: {}] Failed to add the SRT socket to the epoll", static_cast<void *>(group.get()));
        close(sock);
        remove_group(group);
        return false;
    }

    group->set_srt_socket(sock);
    group->set_epoll_fd(epoll_fd_);
    group->write_socket_info_file();
    return true;
}

void SRTHandler::remove_group(connection::ConnectionGroupPtr group) {
    registry_.remove_group(group);
}

} // namespace srtla::protocol
