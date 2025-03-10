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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <endian.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <errno.h>

#include <cstring>
#include <cassert>
#include <vector>
#include <algorithm>
#include <fstream>

#include <argparse/argparse.hpp>

#include "main.h"

int srtla_sock;
struct sockaddr srt_addr;
const socklen_t addr_len = sizeof(struct sockaddr);

std::vector<srtla_conn_group_ptr> conn_groups;

/*
Async I/O support
*/
#define MAX_EPOLL_EVENTS 10

int socket_epoll;

int epoll_add(int fd, uint32_t events, void *priv_data) {
  struct epoll_event ev={0};
  ev.events = events;
  ev.data.ptr = priv_data;
  return epoll_ctl(socket_epoll, EPOLL_CTL_ADD, fd, &ev);
}

int epoll_rem(int fd) {
  struct epoll_event ev; // non-NULL for Linux < 2.6.9, however unlikely it is
  return epoll_ctl(socket_epoll, EPOLL_CTL_DEL, fd, &ev);
}

/*
Misc helper functions
*/
int const_time_cmp(const void *a, const void *b, int len) {
  char diff = 0;
  char *ca = (char *)a;
  char *cb = (char *)b;
  for (int i = 0; i < len; i++) {
    diff |= *ca - *cb;
    ca++;
    cb++;
  }

  return diff ? -1 : 0;
}

inline std::vector<char> get_random_bytes(size_t size)
{
  std::vector<char> ret;
  ret.resize(size);

  std::ifstream f("/dev/urandom");
  f.read(ret.data(), size);
  assert(f); // Failed to read fully!
  f.close();

  return ret;
}

uint16_t get_sock_local_port(int fd)
{
  struct sockaddr_in local_addr = {};
  socklen_t local_addr_len = sizeof(local_addr);
  getsockname(fd, (struct sockaddr *)&local_addr, &local_addr_len);
  return ntohs(local_addr.sin_port);
}

inline void srtla_send_reg_err(struct sockaddr *addr)
{
  uint16_t header = htobe16(SRTLA_TYPE_REG_ERR);
  sendto(srtla_sock, &header, sizeof(header), 0, addr, addr_len);
}

/*
Connection and group management functions
*/
srtla_conn_group_ptr group_find_by_id(char *id) {
  for (auto &group : conn_groups) {
    if (const_time_cmp(group->id.begin(), id, SRTLA_ID_LEN) == 0)
      return group;
  }
  return nullptr;
}

/**
 * Selects the best connection for data transmission based on various metrics
 * 
 * The function performs the following steps:
 * 1. Update capacity estimates
 * 2. Identify active connections
 * 3. Fall back to recovery connections if needed
 * 4. Select the optimal connection based on load/capacity
 * 
 * @param group The connection group
 * @return The selected connection or nullptr if none available
 */
srtla_conn_ptr select_best_conn(srtla_conn_group_ptr group) {
  // Basic validation
  if (!group || group->conns.empty())
    return nullptr;

  // Get current time
  time_t current_time;
  get_seconds(&current_time);

  // Update capacity estimates (only performed every 30 seconds)
  update_connection_capacity(group, current_time);
  
  // Connection selection - staged approach
  
  // 1. First look for active connections
  auto active_conns = get_active_connections(group, current_time);
  
  // 2. If no active connections, try connections in recovery mode
  if (active_conns.empty()) {
    active_conns = get_recovery_connections(group);
    
    // Try to find connections anyway
    if (!active_conns.empty()) {
      spdlog::debug("[Group: {}] No active connections, using {} recovery connections", 
                   static_cast<void *>(group.get()), active_conns.size());
    }
  }

  // 3. If still no connections, use fallback strategy
  if (active_conns.empty()) {
    spdlog::warn("[Group: {}] No active or recovery connections, using fallback strategy", 
                static_cast<void *>(group.get()));
    return select_fallback_connection(group, current_time);
  }
  
  // 4. Select connection based on load and capacity
  srtla_conn_ptr selected_conn = select_connection_based_on_load(group, active_conns, current_time);
  
  // 5. Periodically log bandwidth distribution
  log_bandwidth_distribution(group, current_time);
  
  return selected_conn;
}

// Global decay time variable
static time_t g_last_decay_time = 0;

/**
 * Returns the last decay time for consistent calculations
 */
time_t get_last_decay_time() {
  return g_last_decay_time;
}

/**
 * Sets the last decay time as part of capacity update
 */
void set_last_decay_time(time_t new_time) {
  g_last_decay_time = new_time;
}

/**
 * Updates capacity estimates for all connections in a group
 */
void update_connection_capacity(srtla_conn_group_ptr group, time_t current_time) {
  // Access global time variable for consistency
  time_t last_decay = get_last_decay_time();
  
  // Only update every 30 seconds
  if (current_time - last_decay <= 30) {
    return;
  }
  
  // Store time of update
  set_last_decay_time(current_time);
  
  // Update capacity estimates for each connection in parallel
  for (auto &conn : group->conns) {
    // Update capacity
    update_connection_capacity_estimate(conn, current_time);
    
    // Apply exponential decay to accumulated byte counts for fair distribution
    // Halve the sent bytes to gradually discard historical data
    conn->bytes_sent = conn->bytes_sent / 2;

    // Track potentially problematic connections
    track_connection_health(conn, current_time);
  }
  
  spdlog::info("[Group: {}] Applied bandwidth usage decay and updated capacity estimates", 
              static_cast<void *>(group.get()));
}

/**
 * Updates the capacity estimate for a single connection
 */
void update_connection_capacity_estimate(srtla_conn_ptr conn, time_t current_time) {
  // Only update capacity if the connection was active in this period
  if (conn->bytes_this_period > 0) {
    // If this period exceeded the previous maximum, update the capacity estimate
    if (conn->bytes_this_period > conn->max_bytes_per_period) {
      conn->max_bytes_per_period = conn->bytes_this_period;
      conn->last_capacity_update = current_time;
      
      spdlog::debug("[{}:{}] Updated capacity estimate: {:.2f} MB/period", 
                   print_addr(&conn->addr), port_no(&conn->addr), 
                   conn->max_bytes_per_period / 1048576.0);
    }
    
    // Reset counter for this period
    conn->bytes_this_period = 0;
  } else if (conn->max_bytes_per_period > 0 && 
             (current_time - conn->last_capacity_update) > 60) {
    // Progressively reduce capacity estimate for inactive connections
    // This ensures problematic connections are gradually deprioritized
    conn->max_bytes_per_period = conn->max_bytes_per_period * 0.8;
    spdlog::debug("[{}:{}] Reducing capacity estimate due to inactivity: {:.2f} MB/period", 
                 print_addr(&conn->addr), port_no(&conn->addr), 
                 conn->max_bytes_per_period / 1048576.0);
  }
}

/**
 * Tracks the health status of a connection
 */
void track_connection_health(srtla_conn_ptr conn, time_t current_time) {
  if ((current_time - conn->last_rcvd) > (CONN_TIMEOUT / 2)) {
    if (conn->health_status == 0) {
      conn->health_status = current_time;
      conn->successive_failures = 0;
    } else if ((current_time - conn->health_status) > 5) {
      conn->successive_failures++;
      conn->health_status = current_time;
      spdlog::debug("[{}:{}] Connection health deteriorating: {} failures", 
                   print_addr(&conn->addr), port_no(&conn->addr), 
                   conn->successive_failures);
    }
  } else {
    // Connection is healthy, reset problem indicators
    conn->health_status = 0;
    conn->successive_failures = 0;
  }
}

/**
 * Identifies active connections in a group
 */
std::vector<srtla_conn_ptr> get_active_connections(srtla_conn_group_ptr group, time_t current_time) {
  std::vector<srtla_conn_ptr> active_conns;
  
  for (auto &conn : group->conns) {
    // Consider connection active only if:
    // 1. It's within the timeout period
    // 2. It doesn't have too many successive failures
    bool is_active = (conn->last_rcvd + CONN_TIMEOUT) >= current_time && 
                     conn->successive_failures < 3;
    
    if (is_active) {
      active_conns.push_back(conn);
    } else if (conn->successive_failures >= 3) {
      // Log problematic connections that are excluded
      spdlog::warn("[{}:{}] Connection excluded from load balancing due to {} successive failures", 
                  print_addr(&conn->addr), port_no(&conn->addr), 
                  conn->successive_failures);
                  
      // Occasionally try to recover even excluded connections (every 30s)
      if ((current_time % 30) == 0) {
        // Reset to try again after some time
        conn->successive_failures = 2;
        spdlog::info("[{}:{}] Attempting to reintegrate problematic connection", 
                    print_addr(&conn->addr), port_no(&conn->addr));
      }
    }
  }
  
  return active_conns;
}

/**
 * Retrieves connections in recovery mode
 */
std::vector<srtla_conn_ptr> get_recovery_connections(srtla_conn_group_ptr group) {
  std::vector<srtla_conn_ptr> recovery_conns;
  
  for (auto &conn : group->conns) {
    if (conn->recovery_attempts > 0 && conn->recovery_attempts < 5) {
      recovery_conns.push_back(conn);
    }
  }
  
  return recovery_conns;
}

/**
 * Calculates utilization for all active connections
 * 
 * @param group The connection group
 * @param active_conns List of active connections
 * @param current_time Current time
 * @param last_decay Time of last bandwidth measurement (from update_connection_capacity)
 * @return Vector of connection-utilization pairs
 */
std::vector<std::pair<srtla_conn_ptr, double>> calculate_conn_utilization(
    srtla_conn_group_ptr group, 
    const std::vector<srtla_conn_ptr>& active_conns, 
    time_t current_time) {
    
  if (active_conns.empty()) {
    return {};  // Return empty result if no active connections
  }
  
  // Access shared last_decay variable
  time_t last_decay = get_last_decay_time();
  
  // Pre-allocate result vector for efficiency
  std::vector<std::pair<srtla_conn_ptr, double>> conn_utilization;
  conn_utilization.reserve(active_conns.size());
  
  // Calculate time factor for current period estimation (only once)
  double time_factor = std::min(30.0, static_cast<double>(current_time - last_decay)) / 30.0;
  // Avoid division by zero
  if (time_factor < 0.01) time_factor = 0.01;
  
  for (auto &conn : active_conns) {
    // Calculate relative utilization against capacity
    double utilization = 0.0;
    
    if (conn->max_bytes_per_period > 0) {
      // Estimate current period usage
      double estimated_period_usage = conn->bytes_this_period / time_factor;
      
      // Utilization as ratio of current usage to maximum capacity
      utilization = estimated_period_usage / conn->max_bytes_per_period;
      
      // Limit utilization to a reasonable maximum value (200%)
      if (utilization > 2.0) {
        utilization = 2.0;
      }
    }
    
    conn_utilization.emplace_back(conn, utilization);
  }
  
  return conn_utilization;
}

/**
 * Selects the most recently active connection as fallback mechanism
 */
srtla_conn_ptr select_fallback_connection(srtla_conn_group_ptr group, time_t current_time) {
  if (!group || group->conns.empty()) {
    return nullptr;
  }
  
  // Use std::max_element to determine the most recently active connection
  auto newest_conn = std::max_element(group->conns.begin(), group->conns.end(),
      [](const auto& a, const auto& b) {
          return a->last_rcvd < b->last_rcvd;
      });
  
  spdlog::debug("[Group: {}] Fallback: Using most recently active connection [{}:{}] (last active {} seconds ago)",
               static_cast<void *>(group.get()),
               print_addr(&(*newest_conn)->addr), port_no(&(*newest_conn)->addr),
               current_time - (*newest_conn)->last_rcvd);
               
  return *newest_conn;
}

/**
 * Selects a connection based on load
 */
srtla_conn_ptr select_connection_based_on_load(
    srtla_conn_group_ptr group, 
    std::vector<srtla_conn_ptr>& active_conns,
    time_t current_time) {
    
  if (active_conns.empty()) {
    return nullptr;
  }
  
  static uint64_t round_robin_counter = 0;
  round_robin_counter++;
  
  // Use std::min_element to find the least used connection
  auto least_used = std::min_element(active_conns.begin(), active_conns.end(),
      [](const auto& a, const auto& b) {
          return a->bytes_sent < b->bytes_sent;
      });
  
  // Check if connections are approaching their capacity limit (>70% utilized)
  auto conn_utilization = calculate_conn_utilization(group, active_conns, current_time);
  
  // any_of for checking connections at capacity
  bool any_at_capacity = std::any_of(conn_utilization.begin(), conn_utilization.end(),
      [](const auto& pair) {
          if (pair.second > 0.7) {
              spdlog::debug("[{}:{}] Connection at {:.1f}% capacity, adjusting distribution", 
                           print_addr(&pair.first->addr), port_no(&pair.first->addr), 
                           pair.second * 100.0);
              return true;
          }
          return false;
      });
  
  srtla_conn_ptr selected_conn = nullptr;
  
  // Connection selection strategy based on utilization
  if (any_at_capacity && !conn_utilization.empty()) {
    selected_conn = select_based_on_available_capacity(conn_utilization, round_robin_counter);
  } else {
    // Alternate between load balancing (least used) and round-robin
    if (round_robin_counter % 3 == 0 && least_used != active_conns.end()) {
      selected_conn = *least_used;
    } else {
      size_t index = round_robin_counter % active_conns.size();
      selected_conn = active_conns[index];
    }
  }

  // Reset recovery attempts if connection is restored
  if (selected_conn && selected_conn->recovery_attempts > 0) {
    selected_conn->recovery_attempts = 0;
  }
  
  return selected_conn;
}

/**
 * Selects a connection based on available capacity
 */
srtla_conn_ptr select_based_on_available_capacity(
    std::vector<std::pair<srtla_conn_ptr, double>>& conn_utilization,
    uint64_t round_robin_counter) {
    
  // Sort connections by utilization (ascending)
  std::sort(conn_utilization.begin(), conn_utilization.end(), 
            [](const auto &a, const auto &b) { return a.second < b.second; });
  
  // Select one of the least utilized connections to balance load
  // Rotate through the bottom half of utilized connections
  size_t available_conns = conn_utilization.size() / 2;
  if (available_conns == 0) available_conns = 1;  // At least use the lowest
  
  size_t index = round_robin_counter % available_conns;
  auto selected_conn = conn_utilization[index].first;
  
  spdlog::debug("Load balancing: Using connection with {:.1f}% utilization", 
               conn_utilization[index].second * 100.0);
               
  return selected_conn;
}

/**
 * Logs bandwidth distribution periodically
 */
void log_bandwidth_distribution(srtla_conn_group_ptr group, time_t current_time) {
  static time_t last_log = 0;
  
  // Check logging interval and preconditions
  if (current_time - last_log <= 10 || !group || group->conns.empty()) {
    return;
  }
  
  last_log = current_time;
  
  // Sum bytes and count healthy connections
  uint64_t total_bytes = 0;
  int total_healthy_connections = 0;
  
  for (auto &conn : group->conns) {
    total_bytes += conn->bytes_sent;
    if ((conn->last_rcvd + CONN_TIMEOUT) >= current_time && conn->successive_failures < 3) {
      total_healthy_connections++;
    }
  }
  
  if (total_bytes <= 0) {
    spdlog::debug("[Group: {}] No bandwidth data available for logging", static_cast<void *>(group.get()));
    return;
  }
  
  spdlog::debug("Active connections: {}/{}", total_healthy_connections, group->conns.size());
  
  // Get last decay time for utilization calculations (for consistency)
  time_t last_decay = get_last_decay_time();
  
  // Calculate time factor for all connections (only once)
  double time_factor = std::min(30.0, static_cast<double>(current_time - last_decay)) / 30.0;
  if (time_factor < 0.01) time_factor = 0.01;
  
  // Log connection information
  for (auto &conn : group->conns) {
    double percent = (double)conn->bytes_sent / total_bytes * 100.0;
    double kb_sent = conn->bytes_sent / 1024.0;
    
    // Calculate current utilization
    double utilization = 0.0;
    double capacity_mbps = 0.0;
    
    if (conn->max_bytes_per_period > 0) {
      double estimated_period_usage = conn->bytes_this_period / time_factor;
      utilization = estimated_period_usage / conn->max_bytes_per_period;
      
      // Convert capacity to Mbps
      capacity_mbps = conn->max_bytes_per_period * 8.0 / 30000000.0;
    }
    
    // Format health status
    std::string health_status = conn->successive_failures > 0 ? 
                               fmt::format(" | Health issues: {}", conn->successive_failures) : "";
    
    // Log detailed information
    spdlog::debug("[{}:{}] Bandwidth: {:.1f}% ({:.2f} KB) | Capacity: {:.2f} Mbps | Utilization: {:.1f}%{}",
                 print_addr(&conn->addr), port_no(&conn->addr), 
                 percent, kb_sent, capacity_mbps, utilization * 100.0, health_status);
  }
}

void group_find_by_addr(struct sockaddr *addr, srtla_conn_group_ptr &rg, srtla_conn_ptr &rc) {
  for (auto &group : conn_groups) {
    for (auto &conn : group->conns) {
      if (const_time_cmp(&(conn->addr), addr, addr_len) == 0) {
        rg = group;
        rc = conn;
        return;
      }
    }
    if (const_time_cmp(&group->last_addr, addr, addr_len) == 0) {
      rg = group;
      rc = nullptr;
      return;
    }
  }
  rg = nullptr;
  rc = nullptr;
}

srtla_conn::srtla_conn(struct sockaddr &_addr, time_t ts) :
  addr(_addr),
  last_rcvd(ts),
  max_bytes_per_period(0),
  bytes_this_period(0),
  last_capacity_update(ts)
{
  recv_log.fill(0);
}

srtla_conn_group::srtla_conn_group(char *client_id, time_t ts) :
  created_at(ts)
{
  id.fill(0);

  // Copy client ID to first half of id buffer
  std::memcpy(id.begin(), client_id, SRTLA_ID_LEN / 2);

  // Generate server ID, then copy to last half of id buffer
  auto server_id = get_random_bytes(SRTLA_ID_LEN / 2); 
  std::copy(server_id.begin(), server_id.end(), id.begin() + (SRTLA_ID_LEN / 2));
}

srtla_conn_group::~srtla_conn_group()
{
  conns.clear();

  if (srt_sock > 0) {
    remove_socket_info_file();
    epoll_rem(srt_sock);
    close(srt_sock);
  }
}

std::vector<struct sockaddr> srtla_conn_group::get_client_addresses()
{
  std::vector<struct sockaddr> ret;
  for (auto conn : conns)
    ret.emplace_back(conn->addr);
  return ret;
}

void srtla_conn_group::write_socket_info_file()
{
  if (srt_sock == -1)
    return;

  uint16_t local_port = get_sock_local_port(srt_sock);
  std::string file_name = std::string(SRT_SOCKET_INFO_PREFIX) + std::to_string(local_port);

  auto client_addresses = get_client_addresses();

  std::ofstream f(file_name);
  for (auto &addr : client_addresses)
    f << print_addr(&addr) << std::endl;
  f.close();

  spdlog::debug("[Group: {}] Wrote SRTLA socket info file", static_cast<void *>(this));
}

void srtla_conn_group::remove_socket_info_file()
{
  if (srt_sock == -1)
    return;

  uint16_t local_port = get_sock_local_port(srt_sock);
  std::string file_name = std::string(SRT_SOCKET_INFO_PREFIX) + std::to_string(local_port);

  std::remove(file_name.c_str());
}

int register_group(struct sockaddr *addr, char *in_buf, time_t ts) {
  if (conn_groups.size() >= MAX_GROUPS) {
    srtla_send_reg_err(addr);
    spdlog::error("[{}:{}] Group registration failed: Max groups reached", print_addr(addr), port_no(addr));
    return -1;
  }

  // If this remote address is already registered, abort
  srtla_conn_group_ptr group;
  srtla_conn_ptr conn;
  group_find_by_addr(addr, group, conn);
  if (group) {
    srtla_send_reg_err(addr);
    spdlog::error("[{}:{}] Group registration failed: Remote address already registered to group", print_addr(addr), port_no(addr));
    return -1;
  }

  // Allocate the group
  char *client_id = in_buf + 2;
  group = std::make_shared<srtla_conn_group>(client_id, ts);

  /* Record the address used to register the group
     It won't be allowed to register another group while this one is active */
  group->last_addr = *addr;

  // Build a REG2 packet
  char out_buf[SRTLA_TYPE_REG2_LEN];
  uint16_t header = htobe16(SRTLA_TYPE_REG2);
  std::memcpy(out_buf, &header, sizeof(header));
  std::memcpy(out_buf + sizeof(header), group->id.begin(), SRTLA_ID_LEN);

  // Send the REG2 packet
  int ret = sendto(srtla_sock, &out_buf, sizeof(out_buf), 0, addr, addr_len);
  if (ret != sizeof(out_buf)) {
    spdlog::error("[{}:{}] Group registration failed: Send error", print_addr(addr), port_no(addr));
    return -1;
  }

  conn_groups.push_back(group);

  spdlog::info("[{}:{}] [Group: {}] Group registered", print_addr(addr), port_no(addr), static_cast<void *>(group.get()));
  return 0;
}

void remove_group(srtla_conn_group_ptr group)
{
  if (!group)
    return;

  conn_groups.erase(std::remove(conn_groups.begin(), conn_groups.end(), group), conn_groups.end());

  group.reset();
}

int conn_reg(struct sockaddr *addr, char *in_buf, time_t ts) {
  char *id = in_buf + 2;
  srtla_conn_group_ptr group = group_find_by_id(id);
  if (!group) {
    uint16_t header = htobe16(SRTLA_TYPE_REG_NGP);
    sendto(srtla_sock, &header, sizeof(header), 0, addr, addr_len);
    spdlog::error("[{}:{}] Connection registration failed: No group found", print_addr(addr), port_no(addr));
    return -1;
  }

  /* If the connection is already registered, we'll allow it to register
     again to the same group, but not to a new one */
  srtla_conn_group_ptr tmp;
  srtla_conn_ptr conn;
  group_find_by_addr(addr, tmp, conn);
  if (tmp && tmp != group) {
    srtla_send_reg_err(addr);
    spdlog::error("[{}:{}] [Group: {}] Connection registration failed: Provided group ID mismatch", print_addr(addr), port_no(addr), static_cast<void *>(group.get()));
    return -1;
  }

  /* If the connection is already registered to the group, we can
     just skip ahead to sending the SRTLA_REG3 */
  bool already_registered = true;
  if (!conn) {
    if (group->conns.size() >= MAX_CONNS_PER_GROUP) {
      srtla_send_reg_err(addr);
      spdlog::error("[{}:{}] [Group: {}] Connection registration failed: Max group conns reached", print_addr(addr), port_no(addr), static_cast<void *>(group.get()));
      return -1;
    }

    conn = std::make_shared<srtla_conn>(*addr, ts);
    already_registered = false;
  }

  uint16_t header = htobe16(SRTLA_TYPE_REG3);
  int ret = sendto(srtla_sock, &header, sizeof(header), 0, addr, addr_len);
  if (ret != sizeof(header)) {
    spdlog::error("[{}:{}] [Group: {}] Connection registration failed: Socket send error", print_addr(addr), port_no(addr), static_cast<void *>(group.get()));
    return -1;
  }

  if (!already_registered)
    group->conns.push_back(conn);

  group->write_socket_info_file();

  // If it all worked, mark this peer as the most recently active one
  group->last_addr = *addr;

  spdlog::info("[{}:{}] [Group: {}] Connection registration", print_addr(addr), port_no(addr), static_cast<void *>(group.get()));
  return 0;
}

/*
The main network event handlers
*/
void handle_srt_data(srtla_conn_group_ptr g) {
  char buf[MTU];

  if (!g)
    return;

  int n = recv(g->srt_sock, &buf, MTU, 0);
  if (n < SRT_MIN_LEN) {
    spdlog::error("[Group: {}] Failed to read the SRT sock, terminating the group", static_cast<void *>(g.get()));
    remove_group(g);
    return;
  }

  // ACK
  if (is_srt_ack(buf, n)) {
    // Broadcast SRT ACKs over all connections for timely delivery
    for (auto &conn : g->conns) {
      int ret = sendto(srtla_sock, &buf, n, 0, &conn->addr, addr_len);
      if (ret != n)
        spdlog::error("[{}:{}] [Group: {}] Failed to send the SRT ack", print_addr(&conn->addr), port_no(&conn->addr), static_cast<void *>(g.get()));
    }
  } else {
    srtla_conn_ptr best_conn = select_best_conn(g);
    if (best_conn) {
      int ret = sendto(srtla_sock, &buf, n, 0, &best_conn->addr, addr_len);
      if (ret != n) {
        spdlog::error("[{}:{}] [Group: {}] Failed to send the SRT packet", 
                      print_addr(&best_conn->addr), port_no(&best_conn->addr), 
                      static_cast<void *>(g.get()));
      } else {
        // Update bandwidth statistics based on actual packet size
        best_conn->bytes_sent += n;
        // Also track bytes in current measurement period for capacity estimation
        best_conn->bytes_this_period += n;
      }
    } else {
      int ret = sendto(srtla_sock, &buf, n, 0, &g->last_addr, addr_len);
      if (ret != n) {
        spdlog::error("[{}:{}] [Group: {}] Failed to send the SRT packet", 
                     print_addr(&g->last_addr), port_no(&g->last_addr), 
                     static_cast<void *>(g.get()));
      }
    }
  }
}

void register_packet(srtla_conn_group_ptr group, srtla_conn_ptr conn, int32_t sn) {
  // store the sequence numbers in BE, as they're transmitted over the network
  conn->recv_log[conn->recv_idx++] = htobe32(sn);

  if (conn->recv_idx == RECV_ACK_INT) {
    srtla_ack_pkt ack;
    ack.type = htobe32(SRTLA_TYPE_ACK << 16);
    std::memcpy(&ack.acks, conn->recv_log.begin(), sizeof(uint32_t) * conn->recv_log.max_size());

    int ret = sendto(srtla_sock, &ack, sizeof(ack), 0, &conn->addr, addr_len);
    if (ret != sizeof(ack)) {
      spdlog::error("[{}:{}] [Group: {}] Failed to send the SRTLA ACK", print_addr(&conn->addr), port_no(&conn->addr), static_cast<void *>(group.get()));
    }

    conn->recv_idx = 0;
  }
}

void handle_srtla_data(time_t ts) {
  char buf[MTU] = {};

  // Get the packet
  struct sockaddr srtla_addr;
  socklen_t len = addr_len;
  int n = recvfrom(srtla_sock, &buf, MTU, 0, &srtla_addr, &len);
  if (n < 0) {
    spdlog::error("Failed to read an srtla packet");
    return;
  }

  // Handle srtla registration packets
  if (is_srtla_reg1(buf, n)) {
    register_group(&srtla_addr, buf, ts);
    return;
  }

  if (is_srtla_reg2(buf, n)) {
    conn_reg(&srtla_addr, buf, ts);
    return;
  }

  // Check that the peer is a member of a connection group, discard otherwise
  srtla_conn_group_ptr g;
  srtla_conn_ptr c;
  group_find_by_addr(&srtla_addr, g, c);
  if (!g || !c)
    return;

  // Update the connection's use timestamp
  c->last_rcvd = ts;

  // Resend SRTLA keep-alive packets to the sender
  if (is_srtla_keepalive(buf, n)) {
    int ret = sendto(srtla_sock, &buf, n, 0, &srtla_addr, addr_len);
    if (ret != n) {
      spdlog::error("[{}:{}] [Group: {}] Failed to send SRTLA Keepalive", print_addr(&srtla_addr), port_no(&srtla_addr), static_cast<void *>(g.get()));
    }
    return;
  }

  // Check that the packet is large enough to be an SRT packet, discard otherwise
  if (n < SRT_MIN_LEN) return;

  // Record the most recently active peer
  g->last_addr = srtla_addr;

  // Keep track of the received data packets to send SRTLA ACKs
  int32_t sn = get_srt_sn(buf, n);
  if (sn >= 0) {
    register_packet(g, c, sn);
  }

  // Open a connection to the SRT server for the group
  if (g->srt_sock < 0) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
      spdlog::error("[Group: {}] Failed to create an SRT socket", static_cast<void *>(g.get()));
      remove_group(g);
      return;
    }
    g->srt_sock = sock;

    int ret = connect(sock, &srt_addr, addr_len);
    if (ret != 0) {
      spdlog::error("[Group: {}] Failed to connect() to the SRT socket", static_cast<void *>(g.get()));
      remove_group(g);
      return;
    }

    uint16_t local_port = get_sock_local_port(sock);
    spdlog::info("[Group: {}] Created SRT socket. Local Port: {}", static_cast<void *>(g.get()), local_port);

    ret = epoll_add(sock, EPOLLIN, g.get());
    if (ret != 0) {
      spdlog::error("[Group: {}] Failed to add the SRT socket to the epoll", static_cast<void *>(g.get()));
      remove_group(g);
      return;
    }

    // Write file containing association between local port and client IPs
    g->write_socket_info_file();
  }

  int ret = send(g->srt_sock, &buf, n, 0);
  if (ret != n) {
    spdlog::error("[Group: {}] Failed to forward SRTLA packet, terminating the group", static_cast<void *>(g.get()));
    remove_group(g);
  }
}

/*
  Freeing resources

  Groups:
    * new groups with no connection: created_at < (ts - G_TIMEOUT)
    * other groups: when all connections have timed out
  Connections:
    * GC last_rcvd < (ts - CONN_TIMEOUT)
*/
void cleanup_groups_connections(time_t ts) {
  static time_t last_ran = 0;
  if ((last_ran + CLEANUP_PERIOD) > ts)
    return;
  last_ran = ts;

  if (!conn_groups.size())
    return;

  spdlog::debug("Starting a cleanup run...");

  int total_groups = conn_groups.size();
  int total_conns = 0;
  int removed_groups = 0;
  int removed_conns = 0;
  int recovered_conns = 0;

  for (std::vector<srtla_conn_group_ptr>::iterator git = conn_groups.begin(); git != conn_groups.end();) {
    auto group = *git;

    size_t before_conns = group->conns.size();
    total_conns += before_conns;
    for (std::vector<srtla_conn_ptr>::iterator cit = group->conns.begin(); cit != group->conns.end();) {
      auto conn = *cit;

      if ((conn->last_rcvd + (CONN_TIMEOUT * 1.5)) < ts) {
        cit = group->conns.erase(cit);
        removed_conns++;
        spdlog::info("[{}:{}] [Group: {}] Connection removed (timed out)", print_addr(&conn->addr), port_no(&conn->addr), static_cast<void *>(group.get()));
      } else {
        // More aggressive recovery logic:
        // 1. Starts earlier with recovery attempts (1/4 of CONN_TIMEOUT)
        // 2. Allows more recovery attempts (5 instead of 3)
        // 3. Sends multiple keepalive packets with each attempt
        bool should_attempt_recovery = 
            (conn->last_rcvd + (CONN_TIMEOUT / 4)) < ts && (conn->recovery_attempts < 5);
        
        if (should_attempt_recovery) {
          uint16_t header = htobe16(SRTLA_TYPE_KEEPALIVE);
          // Send multiple keepalives to increase success probability
          for (int i = 0; i < 3; i++) {
            int ret = sendto(srtla_sock, &header, sizeof(header), 0, &conn->addr, addr_len);
            if (ret == sizeof(header)) {
              recovered_conns++;
            }
          }
          conn->recovery_attempts++;
          spdlog::debug("[{}:{}] [Group: {}] Attempting to recover connection (attempt {}/5)", 
                       print_addr(&conn->addr), port_no(&conn->addr), 
                       static_cast<void *>(group.get()), conn->recovery_attempts);
        }
        cit++;
      }
    }

    if (!group->conns.size() && (group->created_at + GROUP_TIMEOUT) < ts) {
      git = conn_groups.erase(git);
      removed_groups++;
      spdlog::info("[Group: {}] Group removed (no connections)", static_cast<void *>(group.get()));
    } else {
      if (before_conns != group->conns.size())
        group->write_socket_info_file();
      git++;
    }
  }

  spdlog::debug("Clean up run ended. Counted {} groups and {} connections. Removed {} groups, {} connections, and attempted to recover {} connections", 
               total_groups, total_conns, removed_groups, removed_conns, recovered_conns);
}

// New function: Proactive ping for connection monitoring
void ping_all_connections(time_t ts) {
  static time_t last_ping = 0;
  // Execute ping every 2 seconds
  if ((last_ping + 2) > ts)
    return;
  last_ping = ts;

  if (conn_groups.empty())
    return;

  // Ping all connections to keep them active
  for (auto &group : conn_groups) {
    for (auto &conn : group->conns) {
      // Send keepalive to all connections that were recently active or
      // are inactive for more than 1/3 of the timeout
      if ((ts - conn->last_rcvd) > (CONN_TIMEOUT / 5)) {
        uint16_t header = htobe16(SRTLA_TYPE_KEEPALIVE);
        sendto(srtla_sock, &header, sizeof(header), 0, &conn->addr, addr_len);
        
        if (conn->recovery_attempts > 0) {
          spdlog::debug("[{}:{}] [Group: {}] Probing inactive connection", 
                      print_addr(&conn->addr), port_no(&conn->addr), static_cast<void *>(group.get()));
        }
      }
      
      // For connections with "recovery_attempts > 0", try even harder to restore them
      if (conn->recovery_attempts > 0) {
        // Send multiple keepalives for recovering connections
        for (int i = 0; i < 2; i++) {
          uint16_t header = htobe16(SRTLA_TYPE_KEEPALIVE);
          sendto(srtla_sock, &header, sizeof(header), 0, &conn->addr, addr_len);
        }
      }
    }
  }
}

/*
SRT is connection-oriented and it won't reply to our packets at this point
unless we start a handshake, so we do that for each resolved address

Returns: -1 when an error has been encountered
          0 when the address was resolved but SRT appears unreachable
          1 when the address was resolved and SRT appears reachable
*/
int resolve_srt_addr(const char *host, const char *port) {
  // Let's set up an SRT handshake induction packet
  srt_handshake_t hs_packet = {0};
  hs_packet.header.type = htobe16(SRT_TYPE_HANDSHAKE);
  hs_packet.version = htobe32(4);
  hs_packet.ext_field = htobe16(2);
  hs_packet.handshake_type = htobe32(1);

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  struct addrinfo *srt_addrs;
  int ret = getaddrinfo(host, port, &hints, &srt_addrs);
  if (ret != 0) {
    spdlog::error("Failed to resolve the address: {}:{}", host, port);
    return -1;
  }

  int tmp_sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (tmp_sock < 0) {
    spdlog::error("Failed to create a UDP socket");
    return -1;
  }

  struct timeval to = { .tv_sec = 1, .tv_usec = 0};
  ret = setsockopt(tmp_sock, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
  if (ret != 0) {
    spdlog::error("Failed to set a socket timeout");
    return -1;
  }

  int found = -1;
  for (struct addrinfo *addr = srt_addrs; addr != NULL && found == -1; addr = addr->ai_next) {
    spdlog::info("Trying to connect to SRT at {}:{}...", print_addr(addr->ai_addr), port);

    ret = connect(tmp_sock, addr->ai_addr, addr->ai_addrlen);
    if (ret == 0) {
      ret = send(tmp_sock, &hs_packet, sizeof(hs_packet), 0);
      if (ret == sizeof(hs_packet)) {
        char buf[MTU];
        ret = recv(tmp_sock, &buf, MTU, 0);
        if (ret == sizeof(hs_packet)) {
          spdlog::info("Success");
          srt_addr = *addr->ai_addr;
          found = 1;
        }
      } // ret == sizeof(buf)
    } // ret == 0

    if (found == -1) {
      spdlog::info("Error");
    }
  }
  close(tmp_sock);

  if (found == -1) {
    srt_addr = *srt_addrs->ai_addr;
    spdlog::warn("Failed to confirm that a SRT server is reachable at any address. Proceeding with the first address: {}", print_addr(&srt_addr));
    found = 0;
  }

  freeaddrinfo(srt_addrs);

  return found;
}

int main(int argc, char **argv) {
  argparse::ArgumentParser args("srtla_rec", VERSION);

  args.add_argument("--srtla_port").help("Port to bind the SRTLA socket to").default_value((uint16_t)5000).scan<'d', uint16_t>();
  args.add_argument("--srt_hostname").help("Hostname of the downstream SRT server").default_value(std::string{"127.0.0.1"});
  args.add_argument("--srt_port").help("Port of the downstream SRT server").default_value((uint16_t)4001).scan<'d', uint16_t>();
  args.add_argument("--verbose").help("Enable verbose logging").default_value(false).implicit_value(true);

  try {
		args.parse_args(argc, argv);
	} catch (const std::runtime_error& err) {
		std::cerr << err.what() << std::endl;
		std::cerr << args;
		std::exit(1);
	}

  uint16_t srtla_port = args.get<uint16_t>("--srtla_port");
  std::string srt_hostname = args.get<std::string>("--srt_hostname");
  std::string srt_port = std::to_string(args.get<uint16_t>("--srt_port"));

  if (args.get<bool>("--verbose"))
    spdlog::set_level(spdlog::level::debug);

  // Try to detect if the SRT server is reachable.
  int ret = resolve_srt_addr(srt_hostname.c_str(), srt_port.c_str());
  if (ret < 0) {
    exit(EXIT_FAILURE);
  }

  // We use epoll for event-driven network I/O
  socket_epoll = epoll_create(1000); // the number is ignored since Linux 2.6.8
  if (socket_epoll < 0) {
    spdlog::critical("epoll creation failed");
    exit(EXIT_FAILURE);
  }

  // Set up the listener socket for incoming SRT connections
  srtla_sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (srtla_sock < 0) {
    spdlog::critical("SRTLA socket creation failed");
    exit(EXIT_FAILURE);
  }

  // Set receive buffer size to 32MB
  int rcv_buf = 32 * 1024 * 1024;
  ret = setsockopt(srtla_sock, SOL_SOCKET, SO_RCVBUF, &rcv_buf, sizeof(rcv_buf));
  if (ret < 0) {
    spdlog::critical("Failed to set SRTLA socket receive buffer size");
    exit(EXIT_FAILURE);
  }

  // TODO: IPv6 listener
  struct sockaddr_in listen_addr = {};
  listen_addr.sin_family = AF_INET;
  listen_addr.sin_addr.s_addr = INADDR_ANY;
  listen_addr.sin_port = htons(srtla_port);
  ret = bind(srtla_sock, (const struct sockaddr *)&listen_addr, addr_len);
  if (ret < 0) {
    spdlog::critical("SRTLA socket bind failed");
    exit(EXIT_FAILURE);
  }

  ret = epoll_add(srtla_sock, EPOLLIN, NULL);
  if (ret != 0) {
    spdlog::critical("Failed to add the SRTLA sock to the epoll");
    exit(EXIT_FAILURE);
  }

  spdlog::info("srtla_rec is now running");

  while(true) {
    struct epoll_event events[MAX_EPOLL_EVENTS];
    int eventcnt = epoll_wait(socket_epoll, events, MAX_EPOLL_EVENTS, 1000);

    time_t ts = 0;
    int ret = get_seconds(&ts);
    if (ret != 0)
      spdlog::error("Failed to get the current time");

    size_t group_cnt;
    for (int i = 0; i < eventcnt; i++) {
      group_cnt = conn_groups.size();
      if (events[i].data.ptr == NULL) {
        handle_srtla_data(ts);
      } else {
        auto g = static_cast<srtla_conn_group *>(events[i].data.ptr);
        handle_srt_data(group_find_by_id(g->id.data()));
      }

      /* If we've removed a group due to a socket error, then we might have
         pending events already waiting for us in events[], and now pointing
         to freed() memory. Get an updated list from epoll_wait() */
      if (conn_groups.size() < group_cnt)
        break;
    } // for

    cleanup_groups_connections(ts);
    ping_all_connections(ts);
  }
}

