#pragma once

#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

#include "connection.h"
#include "../receiver_config.h"
#include "../utils/nak_dedup.h"

extern "C" {
#include "../common.h"
}

namespace srtla::connection {

using srtla::utils::NakHashEntry;

class ConnectionGroup {
public:
    ConnectionGroup(const char *client_id, time_t timestamp);
    ~ConnectionGroup();

    const std::array<char, SRTLA_ID_LEN> &id() const { return id_; }

    void add_connection(const ConnectionPtr &conn);
    void remove_connection(const ConnectionPtr &conn);

    std::vector<ConnectionPtr> &connections() { return conns_; }
    const std::vector<ConnectionPtr> &connections() const { return conns_; }

    time_t created_at() const { return created_at_; }

    int srt_socket() const { return srt_sock_; }
    void set_srt_socket(int sock);

    const struct sockaddr_storage &last_address() const { return last_addr_; }
    void set_last_address(const struct sockaddr_storage &addr) { last_addr_ = addr; }

    uint64_t total_target_bandwidth() const { return total_target_bandwidth_; }
    void set_total_target_bandwidth(uint64_t bw) { total_target_bandwidth_ = bw; }

    time_t last_quality_eval() const { return last_quality_eval_; }
    void set_last_quality_eval(time_t ts) { last_quality_eval_ = ts; }

    time_t last_load_balance_eval() const { return last_load_balance_eval_; }
    void set_last_load_balance_eval(time_t ts) { last_load_balance_eval_ = ts; }
 
    bool load_balancing_enabled() const { return load_balancing_enabled_; }
    void set_load_balancing_enabled(bool enabled) { load_balancing_enabled_ = enabled; }


    std::unordered_map<uint64_t, NakHashEntry> &nak_cache() { return nak_seen_hash_; }

    std::vector<struct sockaddr_storage> get_client_addresses() const;
    void write_socket_info_file() const;
    void remove_socket_info_file() const;

    void set_epoll_fd(int fd) { epoll_fd_ = fd; }

private:
    std::array<char, SRTLA_ID_LEN> id_ {};
    std::vector<ConnectionPtr> conns_;
    time_t created_at_ = 0;
    int srt_sock_ = -1;
    struct sockaddr_storage last_addr_ {};

    uint64_t total_target_bandwidth_ = 0;
    time_t last_quality_eval_ = 0;
    time_t last_load_balance_eval_ = 0;
    bool load_balancing_enabled_ = true;


    std::unordered_map<uint64_t, NakHashEntry> nak_seen_hash_;
    int epoll_fd_ = -1;
};

using ConnectionGroupPtr = std::shared_ptr<ConnectionGroup>;

} // namespace srtla::connection
