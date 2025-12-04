#include "network_utils.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <fstream>

#include <spdlog/spdlog.h>

extern "C" {
#include "../common.h"
}

namespace srtla::utils {

int NetworkUtils::epoll_add(int epoll_fd, int socket_fd, uint32_t events, void *priv_data) {
    struct epoll_event ev {};
    ev.events = events;
    ev.data.ptr = priv_data;
    return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, socket_fd, &ev);
}

int NetworkUtils::epoll_remove(int epoll_fd, int socket_fd) {
    struct epoll_event ev {};
    return epoll_ctl(epoll_fd, EPOLL_CTL_DEL, socket_fd, &ev);
}

uint16_t NetworkUtils::get_local_port(int socket_fd) {
    struct sockaddr_in6 local_addr {};
    socklen_t len = sizeof(local_addr);
    getsockname(socket_fd, reinterpret_cast<struct sockaddr *>(&local_addr), &len);
    return ntohs(local_addr.sin6_port);
}

int NetworkUtils::resolve_srt_address(const char *host,
                                      const char *port,
                                      struct sockaddr_storage *out_addr,
                                      int recv_buf_size,
                                      int send_buf_size) {
    srt_handshake_t hs_packet {};
    hs_packet.header.type = htobe16(SRT_TYPE_HANDSHAKE);
    hs_packet.version = htobe32(4);
    hs_packet.ext_field = htobe16(2);
    hs_packet.handshake_type = htobe32(1);

    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    struct addrinfo *srt_addrs = nullptr;
    int ret = getaddrinfo(host, port, &hints, &srt_addrs);
    if (ret != 0) {
        spdlog::error("Failed to resolve the address: {}:{}: {}", host, port, gai_strerror(ret));
        return -1;
    }

int found = -1;
    int tmp_sock = -1;
    
    for (struct addrinfo *addr = srt_addrs; addr != nullptr && found == -1; addr = addr->ai_next) {
        spdlog::info("Trying to connect to SRT at {}:{}...", print_addr(addr->ai_addr), port);
        
        // Create socket with the appropriate family for this address
        tmp_sock = socket(addr->ai_family, SOCK_DGRAM, 0);
        if (tmp_sock < 0) {
            spdlog::error("Failed to create a UDP socket for family {}", addr->ai_family);
            continue;
        }

        // Set socket options
        bool socket_opts_ok = true;
        if (setsockopt(tmp_sock, SOL_SOCKET, SO_RCVBUF, &recv_buf_size, sizeof(recv_buf_size)) != 0) {
            spdlog::error("Failed to set a receive buffer size ({})", recv_buf_size);
            socket_opts_ok = false;
        }
        if (socket_opts_ok && setsockopt(tmp_sock, SOL_SOCKET, SO_SNDBUF, &send_buf_size, sizeof(send_buf_size)) != 0) {
            spdlog::error("Failed to set a send buffer size ({})", send_buf_size);
            socket_opts_ok = false;
        }
        
        // Set receive timeout to prevent indefinite blocking
        if (socket_opts_ok) {
            struct timeval timeout;
            timeout.tv_sec = 2;  // 2 seconds timeout
            timeout.tv_usec = 0;
            if (setsockopt(tmp_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
                spdlog::error("Failed to set receive timeout");
                socket_opts_ok = false;
            }
        }
        
        if (!socket_opts_ok) {
            close(tmp_sock);
            tmp_sock = -1;
            continue;
        }

        // Connect to the address
        ret = connect(tmp_sock, addr->ai_addr, addr->ai_addrlen);
        if (ret != 0) {
            spdlog::info("Connection failed");
            close(tmp_sock);
            tmp_sock = -1;
            continue;
        }

        // Send handshake packet
        ret = send(tmp_sock, &hs_packet, sizeof(hs_packet), 0);
        if (ret != sizeof(hs_packet)) {
            spdlog::info("Failed to send handshake packet");
            close(tmp_sock);
            tmp_sock = -1;
            continue;
        }

        // Receive response
        char buffer[MTU];
        ret = recv(tmp_sock, &buffer, MTU, 0);
        if (ret == sizeof(hs_packet)) {
            std::memcpy(out_addr, addr->ai_addr, addr->ai_addrlen);
            spdlog::info("Success");
            found = 1;
        } else {
            spdlog::info("Failed to receive handshake response");
            close(tmp_sock);
            tmp_sock = -1;
        }
    }

    if (tmp_sock != -1) {
        close(tmp_sock);
    }

    if (found == -1 && srt_addrs != nullptr) {
        if (srt_addrs->ai_family == AF_INET) {
            std::memcpy(out_addr, srt_addrs->ai_addr, sizeof(struct sockaddr_in));
        } else if (srt_addrs->ai_family == AF_INET6) {
            std::memcpy(out_addr, srt_addrs->ai_addr, sizeof(struct sockaddr_in6));
        }
        spdlog::warn("Failed to confirm that a SRT server is reachable at any address. Proceeding with the first address: {}",
                     print_addr(reinterpret_cast<struct sockaddr *>(out_addr)));
        found = 0;
    }

    freeaddrinfo(srt_addrs);
    return found;
}

int NetworkUtils::constant_time_compare(const void *a, const void *b, int length) {
    const auto *ca = static_cast<const unsigned char *>(a);
    const auto *cb = static_cast<const unsigned char *>(b);
    unsigned char diff = 0;
    for (int i = 0; i < length; ++i) {
        diff |= ca[i] ^ cb[i];
    }
    return diff ? -1 : 0;
}

void NetworkUtils::get_random_bytes(char *buffer, size_t size) {
    std::ifstream random("/dev/urandom", std::ios::in | std::ios::binary);
    random.read(buffer, static_cast<std::streamsize>(size));
    if (!random) {
        spdlog::error("Failed to read {} bytes from /dev/urandom", size);
    }
}

} // namespace srtla::utils
