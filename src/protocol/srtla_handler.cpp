#include "srtla_handler.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <thread>

#include <spdlog/spdlog.h>

extern "C" {
#include "../common.h"
}

namespace srtla::protocol {

using srtla::connection::ConnectionGroupPtr;
using srtla::connection::ConnectionPtr;
using srtla::utils::NakDeduplicator;

namespace {
constexpr socklen_t kAddrLen = sizeof(struct sockaddr_storage);

ConnectionGroupPtr wait_group_by_id(connection::ConnectionRegistry &registry,
                                     const uint8_t *id,
                                     int max_ms = 200) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(max_ms);

    while (clock::now() < deadline) {
        if (auto group = registry.find_group_by_id(reinterpret_cast<const char *>(const_cast<uint8_t *>(id)))) {
            return group;
        }
        std::this_thread::yield();
    }
    return nullptr;
}

bool is_srt_nak_packet(const char *pkt, int length) {
    if (length < static_cast<int>(sizeof(srt_header_t))) {
        return false;
    }
    uint16_t type = get_srt_type(const_cast<char *>(pkt), length);
    return type == SRT_TYPE_NAK;
}

inline bool is_duplicate_nak(ConnectionGroupPtr group, const char *buffer, int length) {
    uint64_t hash = NakDeduplicator::hash_nak_payload(reinterpret_cast<const uint8_t *>(buffer), length, 128);
    uint64_t now_ms = 0;
    get_ms(&now_ms);
    return !NakDeduplicator::should_accept_nak(group->nak_cache(), hash, now_ms);
}

} // namespace

SRTLAHandler::SRTLAHandler(int srtla_socket,
                           connection::ConnectionRegistry &registry,
                           SRTHandler &srt_handler,
                           quality::MetricsCollector &metrics_collector)
    : srtla_socket_(srtla_socket),
      registry_(registry),
      srt_handler_(srt_handler),
      metrics_(metrics_collector) {}

void SRTLAHandler::process_packet(time_t ts) {
    char buf[MTU] = {};
    struct sockaddr_storage srtla_addr {};
    socklen_t len = kAddrLen;

    int n = recvfrom(srtla_socket_, &buf, MTU, 0, reinterpret_cast<struct sockaddr *>(&srtla_addr), &len);
    if (n < 0) {
        spdlog::error("Failed to read an srtla packet {}", strerror(errno));
        return;
    }

    if (is_srtla_reg1(buf, n)) {
        register_group(&srtla_addr, buf, ts);
        return;
    }

    if (is_srtla_reg2(buf, n)) {
        register_connection(&srtla_addr, buf, ts);
        return;
    }

    ConnectionGroupPtr group;
    ConnectionPtr conn;
    registry_.find_by_address(&srtla_addr, group, conn);
    if (!group || !conn) {
        return;
    }

    bool was_timed_out = (conn->last_received() + CONN_TIMEOUT) < ts;
    conn->update_last_received(ts);

    if (conn->recovery_start() == 0 && was_timed_out) {
        conn->set_recovery_start(ts);
        spdlog::info("[{}:{}] [Group: {}] Connection is recovering",
                     print_addr(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&conn->address()))),
                     port_no(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&conn->address()))),
                     static_cast<void *>(group.get()));
    }

    if (is_srtla_keepalive(buf, n)) {
        handle_keepalive(group, conn, &srtla_addr, buf, n);
        return;
    }

    if (n < SRT_MIN_LEN) {
        return;
    }

    group->set_last_address(srtla_addr);
    metrics_.on_packet_received(conn, static_cast<size_t>(n));

    if (is_srt_nak_packet(buf, n)) {
        if (is_duplicate_nak(group, buf, n)) {
            spdlog::info("[{}:{}] [Group: {}] Duplicate NAK packet suppressed",
                         print_addr(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&conn->address()))),
                         port_no(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&conn->address()))),
                         static_cast<void *>(group.get()));
            return;
        }

        metrics_.on_nak_detected(conn, 1);
        spdlog::info("[{}:{}] [Group: {}] Received NAK packet. Total loss: {}",
                     print_addr(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&conn->address()))),
                     port_no(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&conn->address()))),
                     static_cast<void *>(group.get()),
                     conn->stats().packets_lost);

        if (conn->stats().nack_count > 5 && (group->last_quality_eval() + 1) < ts) {
            // quality evaluator will run during cleanup
        }
    }

    int32_t sn = get_srt_sn(buf, n);
    if (sn >= 0) {
        register_packet(group, conn, sn);
    }

    if (!srt_handler_.forward_to_srt_server(group, buf, n)) {
        return;
    }
}

void SRTLAHandler::send_keepalive(const ConnectionPtr &conn, time_t ts) {
    uint16_t pkt = htobe16(SRTLA_TYPE_KEEPALIVE);
    int ret = sendto(srtla_socket_, &pkt, sizeof(pkt), 0,
                     reinterpret_cast<const struct sockaddr *>(&conn->address()), kAddrLen);
    if (ret != sizeof(pkt)) {
        spdlog::error("[{}:{}] Failed to send keepalive packet",
                      print_addr(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&conn->address()))),
                      port_no(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&conn->address()))));
    } else {
        spdlog::debug("[{}:{}] Sent keepalive packet",
                      print_addr(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&conn->address()))),
                      port_no(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&conn->address()))));
    }
}

int SRTLAHandler::register_group(const struct sockaddr_storage *addr, const char *buffer, time_t ts) {
    if (registry_.groups().size() >= MAX_GROUPS) {
        uint16_t header = htobe16(SRTLA_TYPE_REG_ERR);
        sendto(srtla_socket_, &header, sizeof(header), 0,
               reinterpret_cast<const struct sockaddr *>(addr), kAddrLen);
        spdlog::error("[{}:{}] Group registration failed: Max groups reached",
                      print_addr(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(addr))),
                      port_no(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(addr))));
        return -1;
    }

    ConnectionGroupPtr existing_group;
    ConnectionPtr existing_conn;
    registry_.find_by_address(addr, existing_group, existing_conn);
    if (existing_group) {
        uint16_t header = htobe16(SRTLA_TYPE_REG_ERR);
        sendto(srtla_socket_, &header, sizeof(header), 0,
               reinterpret_cast<const struct sockaddr *>(addr), kAddrLen);
        spdlog::error("[{}:{}] Group registration failed: Remote address already registered",
                      print_addr(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(addr))),
                      port_no(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(addr))));
        return -1;
    }

    char *client_id = const_cast<char *>(buffer + 2);
    auto group = std::make_shared<srtla::connection::ConnectionGroup>(client_id, ts);
    group->set_last_address(*addr);

    char out_buf[SRTLA_TYPE_REG2_LEN];
    uint16_t header = htobe16(SRTLA_TYPE_REG2);
    std::memcpy(out_buf, &header, sizeof(header));
    std::memcpy(out_buf + sizeof(header), group->id().data(), SRTLA_ID_LEN);

    int ret = sendto(srtla_socket_, &out_buf, sizeof(out_buf), 0,
                     reinterpret_cast<const struct sockaddr *>(addr), kAddrLen);
    if (ret != sizeof(out_buf)) {
        spdlog::error("[{}:{}] Group registration failed: Send error",
                      print_addr(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(addr))),
                      port_no(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(addr))));
        return -1;
    }

    registry_.add_group(group);
    spdlog::info("[{}:{}] [Group: {}] Group registered",
                 print_addr(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(addr))),
                 port_no(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(addr))),
                 static_cast<void *>(group.get()));
    return 0;
}

int SRTLAHandler::register_connection(const struct sockaddr_storage *addr, const char *buffer, time_t ts) {
    const uint8_t *id = reinterpret_cast<const uint8_t *>(buffer + 2);
    auto group = wait_group_by_id(registry_, id);
    if (!group) {
        uint16_t header = htobe16(SRTLA_TYPE_REG_NGP);
        sendto(srtla_socket_, &header, sizeof(header), 0,
               reinterpret_cast<const struct sockaddr *>(addr), kAddrLen);
        spdlog::error("[{}:{}] Connection registration failed: No group found",
                      print_addr(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(addr))),
                      port_no(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(addr))));
        return -1;
    }

    ConnectionGroupPtr tmp_group;
    ConnectionPtr conn;
    registry_.find_by_address(addr, tmp_group, conn);
    if (tmp_group && tmp_group != group) {
        uint16_t header = htobe16(SRTLA_TYPE_REG_ERR);
        sendto(srtla_socket_, &header, sizeof(header), 0,
               reinterpret_cast<const struct sockaddr *>(addr), kAddrLen);
        spdlog::error("[{}:{}] [Group: {}] Connection registration failed: Provided group ID mismatch",
                      print_addr(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(addr))),
                      port_no(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(addr))),
                      static_cast<void *>(group.get()));
        return -1;
    }

    bool already_registered = true;
    if (!conn) {
        if (group->connections().size() >= MAX_CONNS_PER_GROUP) {
            uint16_t header = htobe16(SRTLA_TYPE_REG_ERR);
            sendto(srtla_socket_, &header, sizeof(header), 0,
                   reinterpret_cast<const struct sockaddr *>(addr), kAddrLen);
            spdlog::error("[{}:{}] [Group: {}] Connection registration failed: Max group conns reached",
                          print_addr(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(addr))),
                          port_no(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(addr))),
                          static_cast<void *>(group.get()));
            return -1;
        }

        conn = std::make_shared<srtla::connection::Connection>(*addr, ts);
        already_registered = false;
    }

    uint16_t header = htobe16(SRTLA_TYPE_REG3);
    int ret = sendto(srtla_socket_, &header, sizeof(header), 0,
                     reinterpret_cast<const struct sockaddr *>(addr), kAddrLen);
    if (ret != sizeof(header)) {
        spdlog::error("[{}:{}] [Group: {}] Connection registration failed: Socket send error",
                      print_addr(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(addr))),
                      port_no(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(addr))),
                      static_cast<void *>(group.get()));
        return -1;
    }

    if (!already_registered) {
        group->add_connection(conn);
    }
    group->write_socket_info_file();
    group->set_last_address(*addr);

    spdlog::info("[{}:{}] [Group: {}] Connection registration",
                 print_addr(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(addr))),
                 port_no(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(addr))),
                 static_cast<void *>(group.get()));
    return 0;
}

void SRTLAHandler::register_packet(ConnectionGroupPtr group,
                                   const ConnectionPtr &conn,
                                   int32_t sn) {
    conn->set_recv_index(conn->recv_index() + 1);
    conn->recv_log()[conn->recv_index() - 1] = htobe32(sn);

    uint64_t current_ms = 0;
    get_ms(&current_ms);

    if (conn->recv_index() == static_cast<int>(RECV_ACK_INT)) {
        bool should_send = true;
        if (conn->stats().ack_throttle_factor < 1.0) {
            uint64_t min_interval = ACK_THROTTLE_INTERVAL / conn->stats().ack_throttle_factor;
            if (conn->stats().last_ack_sent_time > 0 &&
                current_ms < conn->stats().last_ack_sent_time + min_interval) {
                should_send = false;
                spdlog::trace("[{}:{}] [Group: {}] ACK throttled, next in {} ms (factor: {:.2f})",
                              print_addr(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&conn->address()))),
                              port_no(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&conn->address()))),
                              static_cast<void *>(group.get()),
                              (conn->stats().last_ack_sent_time + min_interval) - current_ms,
                              conn->stats().ack_throttle_factor);
            }
        }

        if (should_send) {
            srtla_ack_pkt ack {};
            ack.type = htobe32(SRTLA_TYPE_ACK << 16);
            std::memcpy(&ack.acks, conn->recv_log().data(), sizeof(uint32_t) * conn->recv_log().size());

            int ret = sendto(srtla_socket_, &ack, sizeof(ack), 0,
                             reinterpret_cast<const struct sockaddr *>(&conn->address()), kAddrLen);
            if (ret != sizeof(ack)) {
                spdlog::error("[{}:{}] [Group: {}] Failed to send the SRTLA ACK",
                              print_addr(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&conn->address()))),
                              port_no(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&conn->address()))),
                              static_cast<void *>(group.get()));
            } else {
                conn->stats().last_ack_sent_time = current_ms;
                spdlog::trace("[{}:{}] [Group: {}] Sent SRTLA ACK (throttle factor: {:.2f})",
                              print_addr(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&conn->address()))),
                              port_no(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&conn->address()))),
                              static_cast<void *>(group.get()),
                              conn->stats().ack_throttle_factor);
            }
        }

        conn->set_recv_index(0);
    }
}

void SRTLAHandler::handle_keepalive(ConnectionGroupPtr group,
                                    const ConnectionPtr &conn,
                                    const struct sockaddr_storage *addr,
                                    const char *buffer,
                                    int length) {
    // Try to parse extended keepalive with connection info
    connection_info_t info;
    if (parse_keepalive_conn_info(reinterpret_cast<const uint8_t *>(buffer), length, &info)) {
        // Copy values to avoid packed field reference issues
        uint32_t conn_id = info.conn_id;
        int32_t window = info.window;
        int32_t in_flight = info.in_flight;
        uint64_t rtt_us = info.rtt_us;
        uint32_t nak_count = info.nak_count;
        uint32_t bitrate_kbps = info.bitrate_bytes_per_sec / 1000;
        
        spdlog::info(
            "[{}:{}] [Group: {}] Uplink telemetry: conn_id={}, window={}, in_flight={}, "
            "rtt={}us, naks={}, bitrate={}KB/s",
            print_addr(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(addr))),
            port_no(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(addr))),
            static_cast<void *>(group.get()),
            conn_id,
            window,
            in_flight,
            rtt_us,
            nak_count,
            bitrate_kbps
        );
    }
    
    // Echo the keepalive back to the sender
    int ret = sendto(srtla_socket_, buffer, length, 0,
                     reinterpret_cast<const struct sockaddr *>(addr), kAddrLen);
    if (ret != length) {
        spdlog::error("[{}:{}] [Group: {}] Failed to send SRTLA Keepalive",
                      print_addr(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(addr))),
                      port_no(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(addr))),
                      static_cast<void *>(group.get()));
    }
}

} // namespace srtla::protocol
