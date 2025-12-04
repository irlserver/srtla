#include "connection_group.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "../utils/network_utils.h"

namespace srtla::connection {

using srtla::utils::NetworkUtils;

ConnectionGroup::ConnectionGroup(const char *client_id, time_t timestamp)
    : created_at_(timestamp) {
    id_.fill(0);
    std::memcpy(id_.data(), client_id, SRTLA_ID_LEN / 2);

    char random_bytes[SRTLA_ID_LEN / 2];
    NetworkUtils::get_random_bytes(random_bytes, sizeof(random_bytes));
    std::copy(random_bytes,
              random_bytes + (SRTLA_ID_LEN / 2),
              id_.begin() + (SRTLA_ID_LEN / 2));
}

ConnectionGroup::~ConnectionGroup() {
    conns_.clear();

    if (srt_sock_ > 0) {
        remove_socket_info_file();
        if (epoll_fd_ >= 0) {
            NetworkUtils::epoll_remove(epoll_fd_, srt_sock_);
        }
        close(srt_sock_);
    }
}

void ConnectionGroup::add_connection(const ConnectionPtr &conn) {
    conns_.push_back(conn);
}

void ConnectionGroup::remove_connection(const ConnectionPtr &conn) {
    conns_.erase(std::remove(conns_.begin(), conns_.end(), conn), conns_.end());
}

void ConnectionGroup::set_srt_socket(int sock) {
    srt_sock_ = sock;
}

std::vector<struct sockaddr_storage> ConnectionGroup::get_client_addresses() const {
    std::vector<struct sockaddr_storage> addresses;
    addresses.reserve(conns_.size());
    for (const auto &conn : conns_) {
        addresses.push_back(conn->address());
    }
    return addresses;
}

void ConnectionGroup::write_socket_info_file() const {
    if (srt_sock_ == -1) {
        return;
    }

    uint16_t local_port = NetworkUtils::get_local_port(srt_sock_);
    std::string file_name = std::string(SRT_SOCKET_INFO_PREFIX) + std::to_string(local_port);

    auto client_addresses = get_client_addresses();
    std::ofstream out(file_name);
    for (const auto &addr : client_addresses) {
        auto *mutable_addr = const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&addr));
        out << print_addr(mutable_addr) << std::endl;
    }

    spdlog::info("[Group: {}] Wrote SRTLA socket info file", static_cast<const void *>(this));
}

void ConnectionGroup::remove_socket_info_file() const {
    if (srt_sock_ == -1) {
        return;
    }

    uint16_t local_port = NetworkUtils::get_local_port(srt_sock_);
    std::string file_name = std::string(SRT_SOCKET_INFO_PREFIX) + std::to_string(local_port);
    std::remove(file_name.c_str());

    spdlog::info("[Group: {}] Removed SRTLA socket info file", static_cast<const void *>(this));
}

} // namespace srtla::connection
