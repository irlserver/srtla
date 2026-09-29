#include "prometheus.h"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "../receiver_config.h"
#include "../utils/network_utils.h"

extern "C" {
#include "../common.h"
}

namespace srtla::metrics {

namespace {

uint64_t g_values[COUNTER_COUNT] = {};

struct Def {
    Counter id;
    const char *name;
    const char *labels;
    const char *help;
};

// Ordered by metric name so the HELP/TYPE header is emitted once per name.
// Entries carry their enum id, so this table's order is independent of the
// enum's.
constexpr Def kDefs[] = {
    {AUTH_FAILURES, "srtla_auth_failures_total", "", "SRT authentication failures relayed back from the SRT server"},
    {BYTES_RECEIVED, "srtla_bytes_received_total", "", "Bytes received from SRTLA clients"},
    {CONN_REGISTRATIONS, "srtla_conn_registrations_total", "", "Accepted connection registrations (REG2)"},
    {CONN_REG_NO_GROUP, "srtla_conn_registrations_rejected_total", "{reason=\"no_group\"}", "Rejected connection registrations"},
    {CONN_REG_GROUP_MISMATCH, "srtla_conn_registrations_rejected_total", "{reason=\"group_mismatch\"}", ""},
    {CONN_REG_MAX_CONNS, "srtla_conn_registrations_rejected_total", "{reason=\"max_conns\"}", ""},
    {CONN_REG_SEND_ERROR, "srtla_conn_registrations_rejected_total", "{reason=\"send_error\"}", ""},
    {CONNECTIONS_REMOVED, "srtla_connections_removed_total", "", "Connections dropped after CONN_TIMEOUT without traffic"},
    {FORWARD_DROPPED, "srtla_forward_dropped_total", "", "Client packets dropped because the SRT socket send buffer was full; SRT recovers them when a later packet exposes the gap"},
    {FORWARD_ERRORS, "srtla_forward_errors_total", "", "Failures forwarding a client packet to the SRT server that ended the group"},
    {FORWARDED_BYTES, "srtla_forwarded_bytes_total", "", "Bytes forwarded to the SRT server"},
    {FORWARDED_PACKETS, "srtla_forwarded_packets_total", "", "Packets forwarded to the SRT server"},
    {GROUP_REGISTRATIONS, "srtla_group_registrations_total", "", "Accepted group registrations (REG1)"},
    {GROUP_REG_THROTTLED, "srtla_group_registrations_rejected_total", "{reason=\"throttled\"}", "Rejected group registrations"},
    {GROUP_REG_TABLE_FULL, "srtla_group_registrations_rejected_total", "{reason=\"table_full\"}", ""},
    {GROUP_REG_DUP_ADDR, "srtla_group_registrations_rejected_total", "{reason=\"duplicate_address\"}", ""},
    {GROUP_REG_SEND_ERROR, "srtla_group_registrations_rejected_total", "{reason=\"send_error\"}", ""},
    {GROUPS_REMOVED_IDLE, "srtla_groups_removed_total", "{reason=\"idle_timeout\"}", "Groups torn down, by cause"},
    {GROUPS_REMOVED_AUTH, "srtla_groups_removed_total", "{reason=\"auth_failed\"}", ""},
    {GROUPS_REMOVED_SRT_ERROR, "srtla_groups_removed_total", "{reason=\"srt_error\"}", ""},
    {GROUPS_REMOVED_EVICTED, "srtla_groups_removed_total", "{reason=\"evicted\"}", ""},
    {NAKS_RECEIVED, "srtla_naks_received_total", "", "SRT NAK packets accepted from clients"},
    {NAKS_SUPPRESSED, "srtla_naks_suppressed_total", "", "SRT NAK packets dropped as duplicates"},
    {PACKETS_RECEIVED, "srtla_packets_received_total", "", "Packets received from SRTLA clients"},
    {RECOVERY_STARTED, "srtla_recoveries_total", "{result=\"started\"}", "Connection recovery attempts"},
    {RECOVERY_COMPLETED, "srtla_recoveries_total", "{result=\"completed\"}", ""},
    {RECOVERY_FAILED, "srtla_recoveries_total", "{result=\"failed\"}", ""},
    {RECV_BATCHES, "srtla_recv_batches_total", "", "recvmmsg() calls that returned at least one packet; packets/batches is the receive-loop fill ratio"},
    {SEND_ERR_ACK, "srtla_send_errors_total", "{path=\"ack\"}", "Failed sends towards clients, by path"},
    {SEND_ERR_KEEPALIVE, "srtla_send_errors_total", "{path=\"keepalive\"}", ""},
    {SEND_ERR_DOWNSTREAM, "srtla_send_errors_total", "{path=\"downstream\"}", ""},
    {SRT_BYTES_RECEIVED, "srtla_srt_bytes_received_total", "", "Bytes received from the SRT server"},
    {SRT_PACKETS_RECEIVED, "srtla_srt_packets_received_total", "", "Packets received from the SRT server"},
};

static_assert(sizeof(kDefs) / sizeof(kDefs[0]) == COUNTER_COUNT,
              "every Counter needs exactly one kDefs entry");

struct ConnDef {
    const char *name;
    const char *type;
    const char *help;
    long long (*value)(const connection::Connection &conn, time_t now);
};

constexpr ConnDef kConnDefs[] = {
    {"srtla_conn_bytes_received_total", "counter", "Bytes received on this connection",
     [](const connection::Connection &c, time_t) { return (long long)c.stats().bytes_received; }},
    {"srtla_conn_packets_received_total", "counter", "Packets received on this connection",
     [](const connection::Connection &c, time_t) { return (long long)c.stats().packets_received; }},
    {"srtla_conn_packets_lost_total", "counter", "Packets reported lost (NAKs) on this connection",
     [](const connection::Connection &c, time_t) { return (long long)c.stats().packets_lost; }},
    {"srtla_conn_weight_percent", "gauge", "Load-balancing weight assigned to this connection",
     [](const connection::Connection &c, time_t) { return (long long)c.stats().weight_percent; }},
    {"srtla_conn_error_points", "gauge", "Quality error points accumulated in the last evaluation",
     [](const connection::Connection &c, time_t) { return (long long)c.stats().error_points; }},
    {"srtla_conn_rtt_ms", "gauge", "Round-trip time reported by the sender (extended keepalives only)",
     [](const connection::Connection &c, time_t) { return (long long)c.stats().rtt_ms; }},
    {"srtla_conn_window", "gauge", "Sender-side congestion window (extended keepalives only)",
     [](const connection::Connection &c, time_t) { return (long long)c.stats().window; }},
    {"srtla_conn_in_flight", "gauge", "Sender-side in-flight packets (extended keepalives only)",
     [](const connection::Connection &c, time_t) { return (long long)c.stats().in_flight; }},
    {"srtla_conn_sender_bitrate_bytes_per_second", "gauge", "Send rate reported by the sender (extended keepalives only)",
     [](const connection::Connection &c, time_t) { return (long long)c.stats().sender_bitrate_bps; }},
    {"srtla_conn_idle_seconds", "gauge", "Seconds since the last packet on this connection",
     [](const connection::Connection &c, time_t now) { return (long long)(now - c.last_received()); }},
    {"srtla_conn_extended_keepalives", "gauge", "1 if the sender sends extended keepalives with telemetry",
     [](const connection::Connection &c, time_t) { return (long long)(c.stats().supports_extended_keepalives() ? 1 : 0); }},
};

std::string addr_label(const struct sockaddr_storage &addr) {
    auto *sa = const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&addr));
    return std::string(print_addr(sa)) + ":" + std::to_string(port_no(sa));
}

void append_gauge(std::string &out, const char *name, const char *help, long long value) {
    out += "# HELP ";
    out += name;
    out += ' ';
    out += help;
    out += "\n# TYPE ";
    out += name;
    out += " gauge\n";
    out += name;
    out += ' ';
    out += std::to_string(value);
    out += '\n';
}

} // namespace

void inc(Counter c, uint64_t n) { g_values[c] += n; }

uint64_t value(Counter c) { return g_values[c]; }

Exporter::Exporter(connection::ConnectionRegistry &registry, utils::AuthRateLimiter &rate_limiter)
    : registry_(registry), rate_limiter_(rate_limiter) {}

Exporter::~Exporter() {
    if (listen_fd_ >= 0) {
        close(listen_fd_);
    }
}

bool Exporter::start(const std::string &bind_addr, uint16_t port, int epoll_fd, bool detailed) {
    detailed_ = detailed;
    // Wall clock, not get_seconds(): the exposition convention is a unix
    // timestamp, and get_seconds() returns CLOCK_MONOTONIC_COARSE.
    start_time_ = ::time(nullptr);

    if (port == 0) {
        return false;
    }

    // AI_NUMERICHOST: this is a bind address, not a host to look up, so a typo
    // fails here rather than resolving to an interface nobody intended.
    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;

    struct addrinfo *res = nullptr;
    int gai = getaddrinfo(bind_addr.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (gai != 0) {
        spdlog::error("Metrics: --metrics_bind '{}' is not a numeric address: {}", bind_addr,
                      gai_strerror(gai));
        return false;
    }

    int fd = socket(res->ai_family, SOCK_STREAM, 0);
    if (fd < 0) {
        spdlog::error("Metrics: socket creation failed: {}", strerror(errno));
        freeaddrinfo(res);
        return false;
    }

    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if (res->ai_family == AF_INET6) {
        int off = 0;
        // So :: covers v4 clients too, matching the SRTLA socket.
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
    }

    int flags = fcntl(fd, F_GETFL, 0);
    bool ok = bind(fd, res->ai_addr, res->ai_addrlen) == 0 && listen(fd, 8) == 0 && flags != -1 &&
              fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1 &&
              utils::NetworkUtils::epoll_add(epoll_fd, fd, EPOLLIN, this) == 0;
    freeaddrinfo(res);

    if (!ok) {
        spdlog::error("Metrics: failed to listen on {}:{}: {}", bind_addr, port, strerror(errno));
        close(fd);
        return false;
    }

    listen_fd_ = fd;
    spdlog::info("Metrics endpoint listening on {}:{} (per-connection metrics {})", bind_addr, port,
                 detailed_ ? "enabled" : "disabled");
    return true;
}

void Exporter::handle_event() {
    std::string body;
    bool rendered = false;

    while (true) {
        // SOCK_NONBLOCK: an accepted socket does not inherit it from the
        // listener, and nothing a scraper does may stall the media loop.
        int client = accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK);
        if (client < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                spdlog::warn("Metrics: accept failed: {}", strerror(errno));
            }
            return;
        }

        if (!rendered) {
            time_t now = 0;
            get_seconds(&now);
            body = render(now);
            rendered = true;
        }

        // One read, and only so the request is out of the receive buffer
        // before close(). Every path serves the same body, so the contents do
        // not matter, and looping until EAGAIN would hand a client that keeps
        // sending an unbounded share of this loop.
        char req[512];
        recv(client, req, sizeof(req), 0);

        std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain; version=0.0.4\r\n"
                               "Content-Length: " + std::to_string(body.size()) +
                               "\r\nConnection: close\r\n\r\n" + body;

        // One non-blocking pass. The exposition is a few tens of KB against a
        // socket buffer measured in MB, so a scraper that reads its response
        // gets all of it. A client that stops reading gets a truncated body
        // and a closed socket instead of a share of the media loop.
        size_t sent = 0;
        while (sent < response.size()) {
            ssize_t ret = send(client, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
            if (ret <= 0) {
                if (errno == EINTR) {
                    continue;
                }
                spdlog::debug("Metrics: scrape response truncated at {}/{} bytes: {}", sent,
                              response.size(), strerror(errno));
                break;
            }
            sent += static_cast<size_t>(ret);
        }

        close(client);
    }
}

std::string Exporter::render(time_t now) const {
    std::string out;
    out.reserve(4096);

    const char *prev_name = "";
    for (const auto &def : kDefs) {
        if (std::strcmp(prev_name, def.name) != 0) {
            out += "# HELP ";
            out += def.name;
            out += ' ';
            out += def.help;
            out += "\n# TYPE ";
            out += def.name;
            out += " counter\n";
            prev_name = def.name;
        }
        out += def.name;
        out += def.labels;
        out += ' ';
        out += std::to_string(g_values[def.id]);
        out += '\n';
    }

    const auto &groups = registry_.groups();
    long long established = 0;
    long long connections = 0;
    for (const auto &group : groups) {
        established += group->is_established() ? 1 : 0;
        connections += static_cast<long long>(group->connections().size());
    }

    out += "# HELP srtla_build_info Build information\n# TYPE srtla_build_info gauge\n"
           "srtla_build_info{version=\"" VERSION "\"} 1\n";
    append_gauge(out, "srtla_start_time_seconds", "Unix time the receiver started", start_time_);
    append_gauge(out, "srtla_groups", "Registered connection groups", static_cast<long long>(groups.size()));
    append_gauge(out, "srtla_groups_established", "Groups whose SRT session was accepted by the SRT server", established);
    append_gauge(out, "srtla_connections", "Registered connections across all groups", connections);
    append_gauge(out, "srtla_auth_sources_blocked", "Source IPs currently blocked for repeated SRT auth failures",
                 static_cast<long long>(rate_limiter_.blocked_count(now)));

    if (!detailed_) {
        return out;
    }

    for (const auto &def : kConnDefs) {
        out += "# HELP ";
        out += def.name;
        out += ' ';
        out += def.help;
        out += "\n# TYPE ";
        out += def.name;
        out += ' ';
        out += def.type;
        out += '\n';

        for (const auto &group : groups) {
            const uint16_t group_port = group->srt_socket() >= 0
                                            ? utils::NetworkUtils::get_local_port(group->srt_socket())
                                            : 0;
            for (const auto &conn : group->connections()) {
                out += def.name;
                out += "{group=\"";
                out += std::to_string(group_port);
                out += "\",remote=\"";
                out += addr_label(conn->address());
                out += "\"} ";
                out += std::to_string(def.value(*conn, now));
                out += '\n';
            }
        }
    }

    return out;
}

} // namespace srtla::metrics
