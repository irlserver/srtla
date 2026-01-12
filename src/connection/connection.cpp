#include "connection.h"

#include <cstring>

namespace srtla::connection {

Connection::Connection(const struct sockaddr_storage &addr, time_t timestamp)
    : addr_(addr), last_rcvd_(timestamp), connection_start_(timestamp) {
    recv_log_.fill(0);

    stats_.bytes_received = 0;
    stats_.packets_received = 0;
    stats_.packets_lost = 0;
    stats_.last_eval_time = 0;
    stats_.last_bytes_received = 0;
    stats_.last_packets_received = 0;
    stats_.last_packets_lost = 0;
    stats_.error_points = 0;
    stats_.weight_percent = WEIGHT_FULL;
    stats_.last_ack_sent_time = 0;
    stats_.ack_throttle_factor = 1.0;
    stats_.nack_count = 0;
}

} // namespace srtla::connection
