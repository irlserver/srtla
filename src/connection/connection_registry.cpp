#include "connection_registry.h"

#include <algorithm>

#include <spdlog/spdlog.h>

#include "../receiver_config.h"
#include "../utils/network_utils.h"


extern "C" {
#include "../common.h"
}
namespace srtla::connection {

using srtla::utils::NetworkUtils;

namespace {

bool addresses_equal(const struct sockaddr_storage &a, const struct sockaddr_storage &b) {
    if (a.ss_family != b.ss_family) {
        return false;
    }

    if (a.ss_family == AF_INET6) {
        auto *addr_a = reinterpret_cast<const struct sockaddr_in6 *>(&a);
        auto *addr_b = reinterpret_cast<const struct sockaddr_in6 *>(&b);
        return NetworkUtils::constant_time_compare(&addr_a->sin6_addr, &addr_b->sin6_addr, sizeof(struct in6_addr)) == 0 &&
               addr_a->sin6_port == addr_b->sin6_port;
    }

    auto *addr_a = reinterpret_cast<const struct sockaddr_in *>(&a);
    auto *addr_b = reinterpret_cast<const struct sockaddr_in *>(&b);
    return NetworkUtils::constant_time_compare(&addr_a->sin_addr, &addr_b->sin_addr, sizeof(struct in_addr)) == 0 &&
           addr_a->sin_port == addr_b->sin_port;
}

bool conn_timed_out(const ConnectionPtr &conn, time_t ts) {
    return (conn->last_received() + CONN_TIMEOUT) < ts;
}

} // namespace

ConnectionRegistry &ConnectionRegistry::instance() {
    static ConnectionRegistry registry;
    return registry;
}

void ConnectionRegistry::add_group(const ConnectionGroupPtr &group) {
    groups_.push_back(group);
}

void ConnectionRegistry::remove_group(const ConnectionGroupPtr &group) {
    groups_.erase(std::remove(groups_.begin(), groups_.end(), group), groups_.end());
}

ConnectionGroupPtr ConnectionRegistry::find_group_by_id(const char *id) {
    for (auto &group : groups_) {
        if (NetworkUtils::constant_time_compare(group->id().data(), id, SRTLA_ID_LEN) == 0) {
            return group;
        }
    }
    return nullptr;
}

void ConnectionRegistry::find_by_address(const struct sockaddr_storage *addr,
                                         ConnectionGroupPtr &out_group,
                                         ConnectionPtr &out_conn) {
    for (auto &group : groups_) {
        for (auto &conn : group->connections()) {
            if (addresses_equal(conn->address(), *addr)) {
                out_group = group;
                out_conn = conn;
                return;
            }
        }

        if (addresses_equal(group->last_address(), *addr)) {
            out_group = group;
            out_conn.reset();
            return;
        }
    }

    out_group.reset();
    out_conn.reset();
}

void ConnectionRegistry::cleanup_inactive(time_t current_time,
                                          const std::function<void(ConnectionPtr, time_t)> &keepalive_cb) {
    static time_t last_run = 0;
    if ((last_run + CLEANUP_PERIOD) > current_time) {
        return;
    }
    last_run = current_time;

    if (groups_.empty()) {
        return;
    }

    spdlog::debug("Starting a cleanup run...");

    std::size_t total_groups = groups_.size();
    std::size_t total_connections = 0;
    std::size_t removed_groups = 0;
    std::size_t removed_connections = 0;

    for (auto group_it = groups_.begin(); group_it != groups_.end();) {
        auto group = *group_it;
        std::size_t before_conns = group->connections().size();
        total_connections += before_conns;

        auto &connections = group->connections();
        for (auto conn_it = connections.begin(); conn_it != connections.end();) {
            auto conn = *conn_it;

            if (conn->recovery_start() > 0) {
                if (conn->last_received() > conn->recovery_start()) {
                    if ((current_time - conn->recovery_start()) > RECOVERY_CHANCE_PERIOD) {
                        spdlog::info("[{}:{}] [Group: {}] Connection recovery completed",
                                     print_addr(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&conn->address()))),
                                     port_no(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&conn->address()))),
                                     static_cast<void *>(group.get()));
                        conn->set_recovery_start(0);
                    }
                } else if ((conn->recovery_start() + RECOVERY_CHANCE_PERIOD) < current_time) {
                    spdlog::info("[{}:{}] [Group: {}] Connection recovery failed",
                                 print_addr(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&conn->address()))),
                                 port_no(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&conn->address()))),
                                 static_cast<void *>(group.get()));
                    conn->set_recovery_start(0);
                }
            }

            if (conn_timed_out(conn, current_time)) {
                conn_it = connections.erase(conn_it);
                removed_connections++;
                spdlog::info("[{}:{}] [Group: {}] Connection removed (timed out)",
                             print_addr(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&conn->address()))),
                             port_no(const_cast<struct sockaddr *>(reinterpret_cast<const struct sockaddr *>(&conn->address()))),
                             static_cast<void *>(group.get()));
            } else {
                  if (keepalive_cb && (conn->last_received() + KEEPALIVE_PERIOD) < current_time) {
                    keepalive_cb(conn, current_time);
                }
                ++conn_it;
            }
        }

        if (connections.empty() && (group->created_at() + GROUP_TIMEOUT) < current_time) {
            group_it = groups_.erase(group_it);
            removed_groups++;
            spdlog::info("[Group: {}] Group removed (no connections)", static_cast<void *>(group.get()));
        } else {
            if (before_conns != connections.size()) {
                group->write_socket_info_file();
            }
            ++group_it;
        }
    }

    spdlog::debug("Clean up run ended. Counted {} groups and {} connections. Removed {} groups and {} connections",
                  total_groups, total_connections, removed_groups, removed_connections);
}

} // namespace srtla::connection
