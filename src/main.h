/*
    srtla_rec - SRT transport proxy with link aggregation, forked by IRLToolkit
    Copyright (C) 2020-2021 BELABOX project
    Copyright (C) 2024 IRLToolkit Inc.
    Copyright (C) 2024 OpenIRL

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

#define RECV_ACK_INT 10

#define SRT_SOCKET_INFO_PREFIX "/tmp/srtla-group-"

struct srtla_conn {
    struct sockaddr addr = {};
    time_t last_rcvd = 0;
    int recv_idx = 0;
    std::array<uint32_t, RECV_ACK_INT> recv_log;
    
    uint64_t bytes_sent = 0;
    int recovery_attempts = 0;
    
    // Connection health monitoring
    time_t health_status = 0;     // Time of first health issue
    int successive_failures = 0;  // Number of consecutive issues
    
    // Capacity awareness without high/low categorization
    uint64_t max_bytes_per_period = 0;    // Estimated maximum capacity
    uint64_t bytes_this_period = 0;       // Bytes sent in current period
    time_t last_capacity_update = 0;      // Time of last capacity estimation

    srtla_conn(struct sockaddr &_addr, time_t ts);
};
typedef std::shared_ptr<srtla_conn> srtla_conn_ptr;

struct srtla_conn_group {
    std::array<char, SRTLA_ID_LEN> id;
    std::vector<srtla_conn_ptr> conns;
    time_t created_at = 0;
    int srt_sock = -1;
    struct sockaddr last_addr = {};

    srtla_conn_group(char *client_id, time_t ts);
    ~srtla_conn_group();

    std::vector<struct sockaddr> get_client_addresses();
    void write_socket_info_file();
    void remove_socket_info_file();
};
typedef std::shared_ptr<srtla_conn_group> srtla_conn_group_ptr;

struct srtla_ack_pkt {
    uint32_t type;
    uint32_t acks[RECV_ACK_INT];
};

// Connection selection and management functions
srtla_conn_ptr select_best_conn(srtla_conn_group_ptr group);
srtla_conn_ptr select_connection_strategy(srtla_conn_group_ptr group, time_t current_time);
void update_connection_capacity(srtla_conn_group_ptr group, time_t current_time);
void update_connection_capacity_estimate(srtla_conn_ptr conn, time_t current_time);
void track_connection_health(srtla_conn_ptr conn, time_t current_time);
void handle_slow_connections(srtla_conn_group_ptr group, time_t current_time);
std::vector<srtla_conn_ptr> get_active_connections(srtla_conn_group_ptr group, time_t current_time);
std::vector<srtla_conn_ptr> get_recovery_connections(srtla_conn_group_ptr group);
std::vector<std::pair<srtla_conn_ptr, double>> calculate_conn_utilization(
    srtla_conn_group_ptr group, 
    const std::vector<srtla_conn_ptr>& active_conns, 
    time_t current_time);
srtla_conn_ptr select_connection_based_on_load(
    srtla_conn_group_ptr group, 
    std::vector<srtla_conn_ptr>& active_conns,
    time_t current_time);
srtla_conn_ptr select_based_on_available_capacity(
    std::vector<std::pair<srtla_conn_ptr, double>>& conn_utilization,
    uint64_t round_robin_counter);
srtla_conn_ptr select_fallback_connection(srtla_conn_group_ptr group, time_t current_time);
void log_bandwidth_distribution(srtla_conn_group_ptr group, time_t current_time);

// Time management functions
time_t get_last_decay_time();
void set_last_decay_time(time_t new_time);

// Group management functions
void cleanup_groups_connections(time_t ts);
void ping_all_connections(time_t ts);
