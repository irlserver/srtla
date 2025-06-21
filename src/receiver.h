/*
    srtla_rec - SRT transport proxy with link aggregation
    Copyright (C) 2020-2021 BELABOX project
    Copyright (C) 2024 IRLToolkit Inc.
    Copyright (C) 2024 OpenIRL
    Copyright (C) 2025 IRLServer.com

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#pragma once

#include <memory>

#include <spdlog/spdlog.h>

extern "C" {
#include "common.h"
}

#define MAX_CONNS_PER_GROUP 16
#define MAX_GROUPS          200

#define CLEANUP_PERIOD 3
#define GROUP_TIMEOUT  10
#define CONN_TIMEOUT   10

// Adjustment for Problem 1: Shorter keepalive period for recovery
#define KEEPALIVE_PERIOD 1
#define RECOVERY_CHANCE_PERIOD 5

// Adjustment for Problem 2: Constants for connection quality evaluation
#define CONN_QUALITY_EVAL_PERIOD 5 // Shorter interval for better responsiveness
#define ACK_THROTTLE_INTERVAL 100  // Milliseconds between ACK packets for client control
#define MIN_ACK_RATE 0.2           // Minimum ACK rate (20%) to keep connections alive
#define MIN_ACCEPTABLE_TOTAL_BANDWIDTH_KBPS 1000.0 // Minimum total bandwidth for acceptable streaming quality (1 Mbps)
#define WEIGHT_FULL 100
#define WEIGHT_EXCELLENT 85
#define WEIGHT_DEGRADED 70
#define WEIGHT_FAIR 55
#define WEIGHT_POOR 40
#define WEIGHT_CRITICAL 10

#define RECV_ACK_INT 10

#define SRT_SOCKET_INFO_PREFIX "/tmp/srtla-group-"

struct connection_stats {
    uint64_t bytes_received;         // Received bytes
    uint64_t packets_received;       // Received packets
    uint32_t packets_lost;           // Lost packets (NAKs)
    uint64_t last_eval_time;         // Last evaluation time
    uint64_t last_bytes_received;    // Bytes at last evaluation point
    uint64_t last_packets_received;  // Packets at last evaluation point
    uint32_t last_packets_lost;      // Lost packets at last evaluation point
    uint32_t error_points;           // Error points
    uint8_t weight_percent;          // Weight in percent (0-100)
    uint64_t last_ack_sent_time;     // Timestamp of last ACK packet
    double ack_throttle_factor;      // Factor for throttling ACK frequency (0.1-1.0)
    uint16_t nack_count;             // Number of NAKs in last period
};

struct srtla_conn {
    struct sockaddr_storage addr;
    time_t last_rcvd = 0;
    int recv_idx = 0;
    std::array<uint32_t, RECV_ACK_INT> recv_log;
    
    // Fields for connection quality evaluation
    connection_stats stats = {};
    time_t recovery_start = 0; // Time when the connection began to recover

    srtla_conn(struct sockaddr_storage &_addr, time_t ts);
};
typedef std::shared_ptr<srtla_conn> srtla_conn_ptr;

struct srtla_conn_group {
    std::array<char, SRTLA_ID_LEN> id;
    std::vector<srtla_conn_ptr> conns;
    time_t created_at = 0;
    int srt_sock = -1;
    struct sockaddr_storage last_addr = {};
    
    // Fields for load balancing
    uint64_t total_target_bandwidth = 0; // Total bandwidth
    time_t last_quality_eval = 0;        // Last time of quality evaluation
    bool load_balancing_enabled = true;  // Load balancing enabled

    srtla_conn_group(char *client_id, time_t ts);
    ~srtla_conn_group();

    std::vector<struct sockaddr_storage> get_client_addresses();
    void write_socket_info_file();
    void remove_socket_info_file();
    
    // Methods for load balancing and connection evaluation
    void evaluate_connection_quality(time_t current_time);
    void adjust_connection_weights(time_t current_time);
};
typedef std::shared_ptr<srtla_conn_group> srtla_conn_group_ptr;

struct srtla_ack_pkt {
    uint32_t type;
    uint32_t acks[RECV_ACK_INT];
};

void send_keepalive(srtla_conn_ptr c, time_t ts);
bool conn_timed_out(srtla_conn_ptr c, time_t ts);
bool is_srt_nak(void *pkt, int n);

struct conn_bandwidth_info {
    srtla_conn_ptr conn;
    double bandwidth_kbits_per_sec;
    double packet_loss_ratio;
};
