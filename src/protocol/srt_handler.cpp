#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "srt_handler.h"
#include "forward_policy.h"
#include "pad_sendto.h"

#include <arpa/inet.h>

static inline int is_srt_handshake(const void *pkt, int n) {
    if (n < 16) return 0;
    const unsigned char *p = (const unsigned char *)pkt;
    return (p[0] == 0x80) && (p[1] == 0x00);
}
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "../receiver_config.h"

extern "C" {
#include "../common.h"
}

namespace srtla::protocol {

namespace {

// A full send buffer comes in bursts (thousands of packets in a second after
// an outage), so log the drops at most once a second with a count rather than
// once per packet from the media loop.
constexpr uint64_t kForwardDropLogIntervalMs = 1000;

// Counts a dropped forward and logs at most once per kForwardDropLogIntervalMs,
// reporting how many were dropped since the previous line.
void log_forward_drop(const void *group, int err) {
    static uint64_t dropped_since_log = 0;
    static uint64_t last_log_ms = 0;
    dropped_since_log++;

    uint64_t now_ms = 0;
    if (get_ms(&now_ms) != 0) {
        return;
    }
    if (last_log_ms != 0 && now_ms - last_log_ms < kForwardDropLogIntervalMs) {
        return;
    }
    spdlog::warn("[Group: {}] SRT socket send buffer full ({}): dropping packets instead of ending the group, "
                 "SRT recovers them once later packets reveal the gap; {} dropped since the last report (running total: "
                 "srtla_forward_dropped_total). If this repeats, raise net.core.wmem_max",
                 group, strerror(err), dropped_since_log);
    dropped_since_log = 0;
    last_log_ms = now_ms;
}

} // namespace

SRTHandler::SRTHandler(int srtla_socket,
                       const struct sockaddr_storage &srt_addr,
                       int epoll_fd,
                       connection::ConnectionRegistry &registry,
                       utils::AuthRateLimiter &rate_limiter)
    : srtla_socket_(srtla_socket), srt_addr_(srt_addr), epoll_fd_(epoll_fd),
      registry_(registry), rate_limiter_(rate_limiter) {}

void SRTHandler::handle_srt_data(connection::ConnectionGroupPtr group) {
    if (!group) {
        return;
    }

    char buf[MTU];
    int n = recv(group->srt_socket(), buf, MTU, 0);
    if (n >= SRT_MIN_LEN) {
        metrics::inc(metrics::SRT_PACKETS_RECEIVED);
        metrics::inc(metrics::SRT_BYTES_RECEIVED, static_cast<uint64_t>(n));
    }
    if (n < SRT_MIN_LEN) {
        spdlog::error("[Group: {}] Failed to read the SRT sock, terminating the group",
                      static_cast<void *>(group.get()));
        remove_group(group);
        return;
    }

    // An SRT ACK from the server means media is flowing: the connection was
    // accepted and stream auth passed. Mark the group established so a later
    // SHUTDOWN is treated as a normal end-of-stream rather than a rejection.
    if (is_srt_ack(buf, n)) {
        group->mark_established();
    }

    // Detect a failed/rejected connection and throttle the source IP. Two
    // shapes: a libsrt-native handshake rejection (type >= failure base), or
    // — as srt-live-server does — the server accepts the handshake then closes
    // the socket (SHUTDOWN) before the session is established because stream
    // auth failed. We still relay the packet below so the client sees it, then
    // tear the group down (see end of function).
    bool failed_auth = is_srt_handshake_reject(buf, n) ||
                       (is_srt_shutdown(buf, n) && !group->is_established());
    if (failed_auth) {
        rate_limiter_.record_failure(group->last_address(), ::time(nullptr));
        spdlog::warn("[Group: {}] SRT connection rejected before established; recorded auth failure",
                     static_cast<void *>(group.get()));
    }

    // Broadcast ACKs and NAKs to all connections to ensure they reach the
    // sender even if some connections are dead. Other packets go to last_address.
    if (is_srt_ack(buf, n) || is_srt_nak(buf, n)) {
        const auto &connections = group->connections();
        size_t num_conns = connections.size();

        if (num_conns == 0) {
            return;
        }

        // Use sendmmsg to send to all connections in a single syscall
        struct mmsghdr msgs[MAX_CONNS_PER_GROUP];
        struct iovec iovecs[MAX_CONNS_PER_GROUP];

        size_t msg_count = 0;
        for (const auto &conn : connections) {
            if (msg_count >= MAX_CONNS_PER_GROUP) {
                break;
            }

            iovecs[msg_count].iov_base = buf;
            iovecs[msg_count].iov_len = static_cast<size_t>(n);

            msgs[msg_count].msg_hdr.msg_name = const_cast<struct sockaddr_storage *>(&conn->address());
            msgs[msg_count].msg_hdr.msg_namelen = sizeof(struct sockaddr_storage);
            msgs[msg_count].msg_hdr.msg_iov = &iovecs[msg_count];
            msgs[msg_count].msg_hdr.msg_iovlen = 1;
            msgs[msg_count].msg_hdr.msg_control = nullptr;
            msgs[msg_count].msg_hdr.msg_controllen = 0;
            msgs[msg_count].msg_hdr.msg_flags = 0;

            msg_count++;
        }

        int sent = sendmmsg(srtla_socket_, msgs, static_cast<unsigned int>(msg_count), 0);
        if (sent < 0) {
            metrics::inc(metrics::SEND_ERR_DOWNSTREAM, msg_count);
            spdlog::error("[Group: {}] sendmmsg failed: {}", static_cast<void *>(group.get()), strerror(errno));
        } else if (static_cast<size_t>(sent) < msg_count) {
            metrics::inc(metrics::SEND_ERR_DOWNSTREAM, msg_count - static_cast<size_t>(sent));
            spdlog::warn("[Group: {}] sendmmsg sent only {}/{} messages",
                         static_cast<void *>(group.get()), sent, msg_count);
        }
    } else {
        int ret = pad_sendto(srtla_socket_, &buf, n, 0,
                         reinterpret_cast<const struct sockaddr *>(&group->last_address()), sizeof(struct sockaddr_storage));
        if (ret != n) {
            metrics::inc(metrics::SEND_ERR_DOWNSTREAM);
            spdlog::error("[{}:{}] [Group: {}] Failed to send SRT packet",
                          print_addr(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&group->last_address()))),
                          port_no(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&group->last_address()))),
                          static_cast<void *>(group.get()));
        }
    }

    if (failed_auth) {
        // The client has now been sent the rejection; reclaim the group's slot
        // immediately rather than waiting for it to time out, so repeated
        // failed-auth attempts cannot tie up the group table. Safe here: we
        // hold a strong ref via the by-value `group` param (the object outlives
        // this call), and the main loop stops using stale epoll pointers once
        // the group count shrinks.
        spdlog::info("[Group: {}] Tearing down failed-auth group",
                     static_cast<void *>(group.get()));
        remove_group(group, metrics::GROUPS_REMOVED_AUTH);
    }
}

// Sends one client packet to the SRT server on the group's socket. A transient
// failure drops the packet and keeps the group (forward_policy.h); any other
// failure ends the group. Returns false only when the group was removed.
bool SRTHandler::forward_to_srt_server(connection::ConnectionGroupPtr group, const char *buffer, int length) {
    if (!ensure_group_socket(group)) {
        return false;
    }

    int ret = send(group->srt_socket(), buffer, length, 0);
    if (ret == length) {
        metrics::inc(metrics::FORWARDED_PACKETS);
        metrics::inc(metrics::FORWARDED_BYTES, static_cast<uint64_t>(length));
        return true;
    }

    int err = errno;
    if (ret < 0 && is_transient_forward_error(err)) {
        // Drop this one packet and keep the group: SRT recovers it by
        // retransmission (see forward_policy.h).
        metrics::inc(metrics::FORWARD_DROPPED);
        log_forward_drop(static_cast<void *>(group.get()), err);
        return true;
    }

    metrics::inc(metrics::FORWARD_ERRORS);
    if (ret < 0) {
        spdlog::error("[Group: {}] Failed to forward SRTLA packet ({}), terminating the group",
                      static_cast<void *>(group.get()), strerror(err));
    } else {
        spdlog::error("[Group: {}] Short send forwarding SRTLA packet ({} of {} bytes), terminating the group",
                      static_cast<void *>(group.get()), ret, length);
    }
    remove_group(group);
    return false;
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

void SRTHandler::remove_group(connection::ConnectionGroupPtr group, metrics::Counter reason) {
    metrics::inc(reason);
    registry_.remove_group(group);
}

} // namespace srtla::protocol
