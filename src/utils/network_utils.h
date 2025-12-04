#pragma once

#include <cstdint>
#include <netinet/in.h>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>

namespace srtla::utils {

class NetworkUtils {
public:
    static int epoll_add(int epoll_fd, int socket_fd, uint32_t events, void *priv_data);
    static int epoll_remove(int epoll_fd, int socket_fd);

    static uint16_t get_local_port(int socket_fd);

    static int resolve_srt_address(const char *host,
                                   const char *port,
                                   struct sockaddr_storage *out_addr,
                                   int recv_buf_size,
                                   int send_buf_size);

    static int constant_time_compare(const void *a, const void *b, int length);
    static void get_random_bytes(char *buffer, size_t size);
};

} // namespace srtla::utils
