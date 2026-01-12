#pragma once

#include <array>
#include <memory>
#include <netinet/in.h>
#include <sys/socket.h>

#include "../receiver_config.h"

extern "C" {
#include "../common.h"
}

namespace srtla::connection {

class Connection {
public:
    Connection(const struct sockaddr_storage &addr, time_t timestamp);

    const struct sockaddr_storage &address() const { return addr_; }

    time_t last_received() const { return last_rcvd_; }
    void update_last_received(time_t ts) { last_rcvd_ = ts; }

    int recv_index() const { return recv_idx_; }
    void set_recv_index(int idx) { recv_idx_ = idx; }

    const std::array<uint32_t, RECV_ACK_INT> &recv_log() const { return recv_log_; }
    std::array<uint32_t, RECV_ACK_INT> &recv_log() { return recv_log_; }

    ConnectionStats &stats() { return stats_; }
    const ConnectionStats &stats() const { return stats_; }

    time_t recovery_start() const { return recovery_start_; }
    void set_recovery_start(time_t ts) { recovery_start_ = ts; }

    time_t connection_start() const { return connection_start_; }

    bool extensions_negotiated() const { return extensions_negotiated_; }
    void set_extensions_negotiated(bool negotiated) { extensions_negotiated_ = negotiated; }

    uint32_t sender_capabilities() const { return sender_capabilities_; }
    void set_sender_capabilities(uint32_t caps) { sender_capabilities_ = caps; }

private:
    struct sockaddr_storage addr_ {};
    time_t last_rcvd_ = 0;
    int recv_idx_ = 0;
    std::array<uint32_t, RECV_ACK_INT> recv_log_ {};

    ConnectionStats stats_ {};
    time_t recovery_start_ = 0;
    time_t connection_start_ = 0;

    bool extensions_negotiated_ = false;
    uint32_t sender_capabilities_ = 0;
};

using ConnectionPtr = std::shared_ptr<Connection>;

} // namespace srtla::connection
