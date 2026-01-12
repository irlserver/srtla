#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "connection_group.h"

namespace srtla::connection {

class ConnectionRegistry {
public:
    ConnectionRegistry() = default;

    static ConnectionRegistry &instance();

    void add_group(const ConnectionGroupPtr &group);
    void remove_group(const ConnectionGroupPtr &group);

    ConnectionGroupPtr find_group_by_id(const char *id);
    void find_by_address(const struct sockaddr_storage *addr,
                         ConnectionGroupPtr &out_group,
                         ConnectionPtr &out_conn);

    std::vector<ConnectionGroupPtr> &groups() { return groups_; }
    const std::vector<ConnectionGroupPtr> &groups() const { return groups_; }

    void cleanup_inactive(time_t current_time,
                          const std::function<void(ConnectionPtr, time_t)> &keepalive_cb);

private:
    std::vector<ConnectionGroupPtr> groups_;
};

} // namespace srtla::connection
