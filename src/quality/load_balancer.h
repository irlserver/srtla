#pragma once

#include "../connection/connection_group.h"

namespace srtla::quality {

class LoadBalancer {
public:
    void adjust_weights(connection::ConnectionGroupPtr group, time_t current_time) const;
};

} // namespace srtla::quality
