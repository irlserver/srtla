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

#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <fcntl.h>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <fstream>
#include <vector>
#include <chrono>

#include <argparse/argparse.hpp>

#include "receiver.h"

int srtla_sock;
// Use sockaddr_storage to handle both IPv4 and IPv6
struct sockaddr_storage srt_addr;
const socklen_t addr_len = sizeof(struct sockaddr_storage);

std::vector<srtla_conn_group_ptr> conn_groups;

/*
Async I/O support
*/
#define MAX_EPOLL_EVENTS 10

int socket_epoll;

int epoll_add(int fd, uint32_t events, void *priv_data) {
  struct epoll_event ev = {0};
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

inline std::vector<char> get_random_bytes(size_t size) {
  std::vector<char> ret;
  ret.resize(size);

  std::ifstream f("/dev/urandom");
  f.read(ret.data(), size);
  assert(f); // Failed to read fully!
  f.close();

  return ret;
}

uint16_t get_sock_local_port(int fd) {
  struct sockaddr_in6 local_addr = {};
  socklen_t local_addr_len = sizeof(local_addr);
  getsockname(fd, (struct sockaddr *)&local_addr, &local_addr_len);
  return ntohs(local_addr.sin6_port);
}

inline void srtla_send_reg_err(struct sockaddr_storage *addr) {
  uint16_t header = htobe16(SRTLA_TYPE_REG_ERR);
  sendto(srtla_sock, &header, sizeof(header), 0, (struct sockaddr *)addr,
         addr_len);
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

void group_find_by_addr(struct sockaddr_storage *addr, srtla_conn_group_ptr &rg,
                        srtla_conn_ptr &rc) {
  for (auto &group : conn_groups) {
    for (auto &conn : group->conns) {
      if (conn->addr.ss_family == addr->ss_family &&
          ((conn->addr.ss_family == AF_INET6 &&
            const_time_cmp(&((struct sockaddr_in6 *)(&conn->addr))->sin6_addr,
                           &((struct sockaddr_in6 *)addr)->sin6_addr,
                           sizeof(struct in6_addr)) == 0 &&
            ((struct sockaddr_in6 *)(&conn->addr))->sin6_port ==
                ((struct sockaddr_in6 *)addr)->sin6_port) ||
           (conn->addr.ss_family == AF_INET &&
            const_time_cmp(&((struct sockaddr_in *)(&conn->addr))->sin_addr,
                           &((struct sockaddr_in *)addr)->sin_addr,
                           sizeof(struct in_addr)) == 0 &&
            ((struct sockaddr_in *)(&conn->addr))->sin_port ==
                ((struct sockaddr_in *)addr)->sin_port))) {
        rg = group;
        rc = conn;
        return;
      }
    }
    if (group->last_addr.ss_family == addr->ss_family &&
        ((group->last_addr.ss_family == AF_INET6 &&
          const_time_cmp(
              &((struct sockaddr_in6 *)(&group->last_addr))->sin6_addr,
              &((struct sockaddr_in6 *)addr)->sin6_addr,
              sizeof(struct in6_addr)) == 0 &&
          ((struct sockaddr_in6 *)(&group->last_addr))->sin6_port ==
              ((struct sockaddr_in6 *)addr)->sin6_port) ||
         (group->last_addr.ss_family == AF_INET &&
          const_time_cmp(&((struct sockaddr_in *)(&group->last_addr))->sin_addr,
                         &((struct sockaddr_in *)addr)->sin_addr,
                         sizeof(struct in_addr)) == 0 &&
          ((struct sockaddr_in *)(&group->last_addr))->sin_port ==
              ((struct sockaddr_in *)addr)->sin_port))) {
      rg = group;
      rc = nullptr;
      return;
    }
  }
  rg = nullptr;
  rc = nullptr;
}

srtla_conn::srtla_conn(struct sockaddr_storage &_addr, time_t ts)
    : addr(_addr), last_rcvd(ts) {
  recv_log.fill(0);
  
  // Initialize statistics
  stats.bytes_received = 0;
  stats.packets_received = 0;
  stats.packets_lost = 0;
  stats.last_eval_time = 0;
  stats.last_bytes_received = 0;
  stats.last_packets_received = 0;
  stats.last_packets_lost = 0;
  stats.error_points = 0;
  stats.weight_percent = WEIGHT_FULL; // Start with full weight
  stats.last_ack_sent_time = 0;
  stats.ack_throttle_factor = 1.0;  // Start without throttling
  stats.nack_count = 0;
  
  recovery_start = 0;
}

srtla_conn_group::srtla_conn_group(char *client_id, time_t ts)
    : created_at(ts) {
  id.fill(0);

  // Copy client ID to first half of id buffer
  std::memcpy(id.begin(), client_id, SRTLA_ID_LEN / 2);

  // Generate server ID, then copy to last half of id buffer
  auto server_id = get_random_bytes(SRTLA_ID_LEN / 2);
  std::copy(server_id.begin(), server_id.end(),
            id.begin() + (SRTLA_ID_LEN / 2));
}

srtla_conn_group::~srtla_conn_group() {
  conns.clear();

  if (srt_sock > 0) {
    remove_socket_info_file();
    epoll_rem(srt_sock);
    close(srt_sock);
  }
}

std::vector<struct sockaddr_storage> srtla_conn_group::get_client_addresses() {
  std::vector<struct sockaddr_storage> ret;
  for (auto conn : conns) {
    ret.push_back(conn->addr);
  }
  return ret;
}

void srtla_conn_group::write_socket_info_file() {
  if (srt_sock == -1)
    return;

  uint16_t local_port = get_sock_local_port(srt_sock);
  std::string file_name =
      std::string(SRT_SOCKET_INFO_PREFIX) + std::to_string(local_port);

  auto client_addresses = get_client_addresses();

  std::ofstream f(file_name);
  for (auto &addr : client_addresses)
    f << print_addr((struct sockaddr *)&addr) << std::endl;
  f.close();

  spdlog::debug("[Group: {}] Wrote SRTLA socket info file",
                static_cast<void *>(this));
}

void srtla_conn_group::remove_socket_info_file() {
  if (srt_sock == -1)
    return;

  uint16_t local_port = get_sock_local_port(srt_sock);
  std::string file_name =
      std::string(SRT_SOCKET_INFO_PREFIX) + std::to_string(local_port);

  std::remove(file_name.c_str());
}

int register_group(struct sockaddr_storage *addr, char *in_buf, time_t ts) {
  if (conn_groups.size() >= MAX_GROUPS) {
    srtla_send_reg_err(addr);
    spdlog::error("[{}:{}] Group registration failed: Max groups reached",
                  print_addr((struct sockaddr *)addr),
                  port_no((struct sockaddr *)addr));
    return -1;
  }

  // If this remote address is already registered, abort
  srtla_conn_group_ptr group;
  srtla_conn_ptr conn;
  group_find_by_addr(addr, group, conn);
  if (group) {
    srtla_send_reg_err(addr);
    spdlog::error("[{}:{}] Group registration failed: Remote address already "
                  "registered to group",
                  print_addr((struct sockaddr *)addr),
                  port_no((struct sockaddr *)addr));
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
  int ret = sendto(srtla_sock, &out_buf, sizeof(out_buf), 0,
                   (const sockaddr *)addr, addr_len);
  if (ret != sizeof(out_buf)) {
    spdlog::error("[{}:{}] Group registration failed: Send error",
                  print_addr((struct sockaddr *)addr),
                  port_no((struct sockaddr *)addr));
    return -1;
  }

  conn_groups.push_back(group);

  spdlog::info("[{}:{}] [Group: {}] Group registered",
               print_addr((struct sockaddr *)addr),
               port_no((struct sockaddr *)addr),
               static_cast<void *>(group.get()));
  return 0;
}

void remove_group(srtla_conn_group_ptr group) {
  if (!group)
    return;

  conn_groups.erase(std::remove(conn_groups.begin(), conn_groups.end(), group),
                    conn_groups.end());

  group.reset();
}

int conn_reg(struct sockaddr_storage *addr, char *in_buf, time_t ts) {
  char *id = in_buf + 2;
  srtla_conn_group_ptr group = group_find_by_id(id);
  if (!group) {
    uint16_t header = htobe16(SRTLA_TYPE_REG_NGP);
    sendto(srtla_sock, &header, sizeof(header), 0, (const sockaddr *)addr,
           addr_len);
    spdlog::error("[{}:{}] Connection registration failed: No group found",
                  print_addr((struct sockaddr *)addr),
                  port_no((struct sockaddr *)addr));
    return -1;
  }

  /* If the connection is already registered, we'll allow it to register
     again to the same group, but not to a new one */
  srtla_conn_group_ptr tmp;
  srtla_conn_ptr conn;
  group_find_by_addr(addr, tmp, conn);
  if (tmp && tmp != group) {
    srtla_send_reg_err(addr);
    spdlog::error("[{}:{}] [Group: {}] Connection registration failed: "
                  "Provided group ID mismatch",
                  print_addr((struct sockaddr *)addr),
                  port_no((struct sockaddr *)addr),
                  static_cast<void *>(group.get()));
    return -1;
  }

  /* If the connection is already registered to the group, we can
     just skip ahead to sending the SRTLA_REG3 */
  bool already_registered = true;
  if (!conn) {
    if (group->conns.size() >= MAX_CONNS_PER_GROUP) {
      srtla_send_reg_err(addr);
      spdlog::error("[{}:{}] [Group: {}] Connection registration failed: Max "
                    "group conns reached",
                    print_addr((struct sockaddr *)addr),
                    port_no((struct sockaddr *)addr),
                    static_cast<void *>(group.get()));
      return -1;
    }

    conn = std::make_shared<srtla_conn>(*addr, ts);
    already_registered = false;
  }

  uint16_t header = htobe16(SRTLA_TYPE_REG3);
  int ret = sendto(srtla_sock, &header, sizeof(header), 0,
                   (const sockaddr *)addr, addr_len);
  if (ret != sizeof(header)) {
    spdlog::error(
        "[{}:{}] [Group: {}] Connection registration failed: Socket send error",
        print_addr((struct sockaddr *)addr), port_no((struct sockaddr *)addr),
        static_cast<void *>(group.get()));
    return -1;
  }

  if (!already_registered)
    group->conns.push_back(conn);

  group->write_socket_info_file();

  // If it all worked, mark this peer as the most recently active one
  group->last_addr = *addr;

  spdlog::info("[{}:{}] [Group: {}] Connection registration",
               print_addr((struct sockaddr *)addr),
               port_no((struct sockaddr *)addr),
               static_cast<void *>(group.get()));
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
    spdlog::error(
        "[Group: {}] Failed to read the SRT sock, terminating the group",
        static_cast<void *>(g.get()));
    remove_group(g);
    return;
  }

  // ACK
  if (is_srt_ack(buf, n)) {
    // Broadcast SRT ACKs over all connections for timely delivery
    for (auto &conn : g->conns) {
      int ret = sendto(srtla_sock, &buf, n, 0, (struct sockaddr *)&conn->addr,
                       addr_len);
      if (ret != n)
        spdlog::error("[{}:{}] [Group: {}] Failed to send the SRT ack",
                      print_addr((struct sockaddr *)&conn->addr),
                      port_no((struct sockaddr *)&conn->addr),
                      static_cast<void *>(g.get()));
    }
  } else {
    // send other packets over the most recently used SRTLA connection
    int ret = sendto(srtla_sock, &buf, n, 0, (struct sockaddr *)&g->last_addr,
                     addr_len);
    if (ret != n) {
      spdlog::error("[{}:{}] [Group: {}] Failed to send the SRT packet",
                    print_addr((struct sockaddr *)&g->last_addr),
                    port_no((struct sockaddr *)&g->last_addr),
                    static_cast<void *>(g.get()));
    }
  }
}

void register_packet(srtla_conn_group_ptr group, srtla_conn_ptr conn,
                     int32_t sn) {
  // store the sequence numbers in BE, as they're transmitted over the network
  conn->recv_log[conn->recv_idx++] = htobe32(sn);

  // Get current time for ACK throttling
  uint64_t current_ms;
  get_ms(&current_ms);

  if (conn->recv_idx == RECV_ACK_INT) {
    bool should_send = true;
    
    // Apply throttling based on time intervals using pre-calculated factor
    if (conn->stats.ack_throttle_factor < 1.0) {
      uint64_t min_interval = ACK_THROTTLE_INTERVAL / conn->stats.ack_throttle_factor;
      
      if (conn->stats.last_ack_sent_time > 0 && 
          current_ms < conn->stats.last_ack_sent_time + min_interval) {
        should_send = false;
        spdlog::trace("[{}:{}] [Group: {}] ACK throttled, next in {} ms (factor: {:.2f})",
                     print_addr((struct sockaddr *)&conn->addr), port_no((struct sockaddr *)&conn->addr), static_cast<void *>(group.get()),
                     (conn->stats.last_ack_sent_time + min_interval) - current_ms,
                     conn->stats.ack_throttle_factor);
      }
    }
    
    if (should_send) {
      srtla_ack_pkt ack;
      ack.type = htobe32(SRTLA_TYPE_ACK << 16);
      std::memcpy(&ack.acks, conn->recv_log.begin(), sizeof(uint32_t) * conn->recv_log.max_size());

      int ret = sendto(srtla_sock, &ack, sizeof(ack), 0, (struct sockaddr *)&conn->addr, addr_len);
      if (ret != sizeof(ack)) {
        spdlog::error("[{}:{}] [Group: {}] Failed to send the SRTLA ACK", 
            print_addr((struct sockaddr *)&conn->addr), port_no((struct sockaddr *)&conn->addr), static_cast<void *>(group.get()));
      } else {
        // Update the timestamp of the last sent ACK
        conn->stats.last_ack_sent_time = current_ms;
        spdlog::trace("[{}:{}] [Group: {}] Sent SRTLA ACK (throttle factor: {:.2f})",
                     print_addr((struct sockaddr *)&conn->addr), port_no((struct sockaddr *)&conn->addr), static_cast<void *>(group.get()),
                     conn->stats.ack_throttle_factor);
      }
    }

    conn->recv_idx = 0;
  }
}

// Add this function for detecting NAK packets
bool is_srt_nak(void *pkt, int n) {
  if (n < sizeof(srt_header_t)) return false;
  uint16_t type = get_srt_type(pkt, n);
  return type == SRT_TYPE_NAK;
}

void handle_srtla_data(time_t ts) {
  char buf[MTU] = {};

  // Get the packet
  struct sockaddr_storage srtla_addr;
  socklen_t len = addr_len;
  int n =
      recvfrom(srtla_sock, &buf, MTU, 0, (struct sockaddr *)&srtla_addr, &len);
  if (n < 0) {
    spdlog::error("Failed to read an srtla packet {}", strerror(errno));
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
  
  // For Problem 1: Set recovery_start when the connection is restored
  // When a connection comes back after a timeout, mark it for recovery
  if (c->recovery_start == 0 && (c->last_rcvd == 1 || conn_timed_out(c, ts - 1))) {
    c->recovery_start = ts;
    spdlog::info("[{}:{}] [Group: {}] Connection is recovering", 
                print_addr((struct sockaddr *)&c->addr), port_no((struct sockaddr *)&c->addr), static_cast<void *>(g.get()));
  }

  // Resend SRTLA keep-alive packets to the sender
  if (is_srtla_keepalive(buf, n)) {
    int ret = sendto(srtla_sock, &buf, n, 0, (struct sockaddr *)&srtla_addr,
                     addr_len);
    if (ret != n) {
      spdlog::error("[{}:{}] [Group: {}] Failed to send SRTLA Keepalive",
                    print_addr((struct sockaddr *)&srtla_addr),
                    port_no((struct sockaddr *)&srtla_addr),
                    static_cast<void *>(g.get()));
    }
    return;
  }

  // Check that the packet is large enough to be an SRT packet, discard
  // otherwise
  if (n < SRT_MIN_LEN)
    return;

  // Record the most recently active peer
  g->last_addr = srtla_addr;

  // For Problem 2: Update connection statistics
  c->stats.bytes_received += n;
  c->stats.packets_received++;
  
  // Check for NAK packets to track packet loss
  if (is_srt_nak(buf, n)) {
    c->stats.packets_lost++;
    c->stats.nack_count++;
    
    spdlog::debug("[{}:{}] [Group: {}] Received NAK packet. Total NAKs: {}, Total loss: {}", 
                 print_addr((struct sockaddr *)&c->addr), port_no((struct sockaddr *)&c->addr), static_cast<void *>(g.get()),
                 c->stats.nack_count, c->stats.packets_lost);
    
    // For high NAK rates, re-evaluate connection quality immediately
    if (c->stats.nack_count > 5 && (g->last_quality_eval + 1) < ts) {
      g->evaluate_connection_quality(ts);
    }
  }

  // Keep track of the received data packets to send SRTLA ACKs
  int32_t sn = get_srt_sn(buf, n);
  if (sn >= 0) {
    register_packet(g, c, sn);
  }

  // Open a connection to the SRT server for the group
  if (g->srt_sock < 0) {
    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (sock < 0) {
      spdlog::error("[Group: {}] Failed to create an SRT socket",
                    static_cast<void *>(g.get()));
      remove_group(g);
      return;
    }
    g->srt_sock = sock;

    // Set receive buffer size for g->srt_sock
    int bufsize = RECV_BUF_SIZE;
    int ret = setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    if (ret != 0) {
      spdlog::error("failed to set receive buffer size ({})", bufsize);
      remove_group(g);
      return;
    }

    // Set send buffer size for g->srt_sock
    int sndbufsize = SEND_BUF_SIZE;
    ret = setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbufsize, sizeof(sndbufsize));
    if (ret != 0) {
      spdlog::error("failed to set send buffer size ({})", bufsize);
      remove_group(g);
      return;
    }
  
    // Set g->srt_sock to non-blocking
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags == -1 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) == -1) {
      spdlog::error("failed to set g->srt_sock non-blocking");
      remove_group(g);
      return;
    }

    // Connect using the appropriate address family
    if (srt_addr.ss_family == AF_INET) {
      ret = connect(sock, (struct sockaddr *)&srt_addr,
                    sizeof(struct sockaddr_in));
    } else if (srt_addr.ss_family == AF_INET6) {
      ret = connect(sock, (struct sockaddr *)&srt_addr,
                    sizeof(struct sockaddr_in6));
    } else {
      spdlog::error("[Group: {}] Invalid address family for SRT server",
                    static_cast<void *>(g.get()));
      remove_group(g);
      return;
    }

    uint16_t local_port = get_sock_local_port(sock);
    spdlog::info("[Group: {}] Created SRT socket. Local Port: {}",
                 static_cast<void *>(g.get()), local_port);

    ret = epoll_add(sock, EPOLLIN, g.get());
    if (ret != 0) {
      spdlog::error("[Group: {}] Failed to add the SRT socket to the epoll",
                    static_cast<void *>(g.get()));
      remove_group(g);
      return;
    }

    // Write file containing association between local port and client IPs
    g->write_socket_info_file();
  }

  int ret = send(g->srt_sock, &buf, n, 0);
  if (ret != n) {
    spdlog::error(
        "[Group: {}] Failed to forward SRTLA packet, terminating the group",
        static_cast<void *>(g.get()));
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

  for (std::vector<srtla_conn_group_ptr>::iterator git = conn_groups.begin();
       git != conn_groups.end();) {
    auto group = *git;
    
    // For Problem 2: Evaluate connection quality
    group->evaluate_connection_quality(ts);

    size_t before_conns = group->conns.size();
    total_conns += before_conns;
    for (std::vector<srtla_conn_ptr>::iterator cit = group->conns.begin();
         cit != group->conns.end();) {
      auto conn = *cit;

      // Check if the connection is in recovery mode
      if (conn->recovery_start > 0) {
        // If the connection has received data since recovery started, it's recovering
        if (conn->last_rcvd > conn->recovery_start) {
          if ((ts - conn->recovery_start) > RECOVERY_CHANCE_PERIOD) {
            spdlog::info("[{}:{}] [Group: {}] Connection recovery completed", 
                       print_addr((struct sockaddr *)&conn->addr), port_no((struct sockaddr *)&conn->addr), static_cast<void *>(group.get()));
            conn->recovery_start = 0;
          } else {
            // Send keepalive packets more frequently during the recovery phase
            if ((conn->last_rcvd + KEEPALIVE_PERIOD) < ts) {
              send_keepalive(conn, ts);
            }
          }
        } 
        // If the recovery phase takes too long without success, give up
        else if ((conn->recovery_start + RECOVERY_CHANCE_PERIOD) < ts) {
          spdlog::info("[{}:{}] [Group: {}] Connection recovery failed", 
                     print_addr((struct sockaddr *)&conn->addr), port_no((struct sockaddr *)&conn->addr), static_cast<void *>(group.get()));
          conn->recovery_start = 0;
        }
      }

      if ((conn->last_rcvd + CONN_TIMEOUT) < ts) {
        cit = group->conns.erase(cit);
        removed_conns++;
        spdlog::info("[{}:{}] [Group: {}] Connection removed (timed out)",
                     print_addr((struct sockaddr *)&conn->addr),
                     port_no((struct sockaddr *)&conn->addr),
                     static_cast<void *>(group.get()));
      } else {
        // Send keepalive packets to connections more frequently if they are in recovery mode
        if (conn->recovery_start > 0 && (conn->last_rcvd + KEEPALIVE_PERIOD) < ts) {
          send_keepalive(conn, ts);
        }
        cit++;
      }
    }

    if (!group->conns.size() && (group->created_at + GROUP_TIMEOUT) < ts) {
      git = conn_groups.erase(git);
      removed_groups++;
      spdlog::info("[Group: {}] Group removed (no connections)",
                   static_cast<void *>(group.get()));
    } else {
      if (before_conns != group->conns.size())
        group->write_socket_info_file();
      git++;
    }
  }

  spdlog::debug("Clean up run ended. Counted {} groups and {} connections. "
                "Removed {} groups and {} connections",
                total_groups, total_conns, removed_groups, removed_conns);
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

  struct addrinfo hints, *srt_addrs;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC; // Allow IPv4 or IPv6
  hints.ai_socktype = SOCK_DGRAM;
  int ret = getaddrinfo(host, port, &hints, &srt_addrs);
  if (ret != 0) {
    spdlog::error("Failed to resolve the address: {}:{}: {}", host, port,
                  gai_strerror(ret));
    return -1;
  }

  int tmp_sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (tmp_sock < 0) {
    spdlog::error("Failed to create a UDP socket");
    return -1;
  }

  // Set receive buffer size for tmp_sock
  int bufsize = RECV_BUF_SIZE;
  ret = setsockopt(tmp_sock, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
  if (ret != 0) {
    spdlog::error("Failed to set a receive buffer size ({} bytes)", bufsize);
    return -1;
  }

  // Set send buffer size for tmp_sock
  bufsize = SEND_BUF_SIZE;
  ret = setsockopt(tmp_sock, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
  if (ret != 0) {
    spdlog::error("Failed to set a send buffer size ({} bytes)", bufsize);
    return -1;
  }

  int found = -1;
  for (struct addrinfo *addr = srt_addrs; addr != NULL && found == -1;
       addr = addr->ai_next) {
    spdlog::info("Trying to connect to SRT at {}:{}...",
                 print_addr((struct sockaddr *)addr->ai_addr), port);
    if (addr->ai_family == AF_INET) {
      ret = connect(tmp_sock, addr->ai_addr, sizeof(struct sockaddr_in));
    } else if (addr->ai_family == AF_INET6) {
      ret = connect(tmp_sock, addr->ai_addr, sizeof(struct sockaddr_in6));
    } else {
      spdlog::warn("Unsupported address family, skipping");
      continue;
    }
    if (ret == 0) {
      ret = send(tmp_sock, &hs_packet, sizeof(hs_packet), 0);
      if (ret == sizeof(hs_packet)) {
        char buf[MTU];
        ret = recv(tmp_sock, &buf, MTU, 0);
        if (ret == sizeof(hs_packet)) {
          spdlog::info("Success");
          // Copy the successful address to srt_addr
          if (addr->ai_family == AF_INET) {
            memcpy(&srt_addr, addr->ai_addr, sizeof(struct sockaddr_in));
          } else {
            // AF_INET6
            memcpy(&srt_addr, addr->ai_addr, sizeof(struct sockaddr_in6));
          }
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
    // If no successful connection, default to the first address
    if (srt_addrs->ai_family == AF_INET) {
      memcpy(&srt_addr, srt_addrs->ai_addr, sizeof(struct sockaddr_in));
    } else if (srt_addrs->ai_family == AF_INET6) {
      memcpy(&srt_addr, srt_addrs->ai_addr, sizeof(struct sockaddr_in6));
    }
    spdlog::warn("Failed to confirm that a SRT server is reachable at any "
                 "address. Proceeding with the first address: {}",
                 print_addr((struct sockaddr *)&srt_addr));
    found = 0;
  }

  freeaddrinfo(srt_addrs);

  return found;
}

// Implementation of the new functions for connection quality assessment
void srtla_conn_group::evaluate_connection_quality(time_t current_time) {
    if (conns.empty() || !load_balancing_enabled)
        return;
        
    if (last_quality_eval + CONN_QUALITY_EVAL_PERIOD > current_time)
        return;
        
    spdlog::debug("[Group: {}] Evaluating connection quality", static_cast<void *>(this));
    
    // First pass - calculate total bandwidth and gather basic stats
    total_target_bandwidth = 0;
    uint64_t current_ms;
    get_ms(&current_ms);
    
    std::vector<conn_bandwidth_info> bandwidth_info;

    // First pass - calculate raw bandwidth for each connection
    for (auto &conn : conns) {
        // Time since last evaluation
        uint64_t time_diff_ms = 0;
        if (conn->stats.last_eval_time > 0) {
            time_diff_ms = current_ms - conn->stats.last_eval_time;
        }
        
        if (time_diff_ms > 0) {
            // Calculate metrics from the last period
            uint64_t bytes_diff = conn->stats.bytes_received - conn->stats.last_bytes_received;
            uint64_t packets_diff = conn->stats.packets_received - conn->stats.last_packets_received;
            uint32_t lost_diff = conn->stats.packets_lost - conn->stats.last_packets_lost;
            
            // Calculate bandwidth in bytes/sec
            double seconds = static_cast<double>(time_diff_ms) / 1000.0;
            double bandwidth_bytes_per_sec = bytes_diff / seconds;

            // Calculate bandwidth in kbits/sec for more intuitive evaluation
            double bandwidth_kbits_per_sec = (bandwidth_bytes_per_sec * 8.0) / 1000.0;
            
            // Calculate packet loss ratio
            double packet_loss_ratio = 0;
            if (packets_diff > 0) {
                packet_loss_ratio = static_cast<double>(lost_diff) / (packets_diff + lost_diff);
            }
            
            // Store bandwidth info for this connection
            bandwidth_info.push_back({conn, bandwidth_kbits_per_sec, packet_loss_ratio});

            // Update total bandwidth
            total_target_bandwidth += static_cast<uint64_t>(bandwidth_bytes_per_sec);
        }

        // Store current values for next evaluation
        conn->stats.last_bytes_received = conn->stats.bytes_received;
        conn->stats.last_packets_received = conn->stats.packets_received;
        conn->stats.last_packets_lost = conn->stats.packets_lost;
        conn->stats.last_eval_time = current_ms;
    }

    // Skip further processing if we don't have enough data
    if (bandwidth_info.empty())
        return;

    // Calculate total bandwidth and find the best performing connection
    double total_kbits_per_sec = (total_target_bandwidth * 8.0) / 1000.0;
    double max_kbits_per_sec = 0.0;
    double median_kbits_per_sec = 0.0;
    
    // Find maximum bandwidth to use as reference for good connections
    std::vector<double> all_bandwidths;
    for (const auto &info : bandwidth_info) {
        all_bandwidths.push_back(info.bandwidth_kbits_per_sec);
        max_kbits_per_sec = std::max(max_kbits_per_sec, info.bandwidth_kbits_per_sec);
    }
    
    // Calculate median bandwidth for more robust reference
    if (!all_bandwidths.empty()) {
        std::sort(all_bandwidths.begin(), all_bandwidths.end());
        size_t mid = all_bandwidths.size() / 2;
        median_kbits_per_sec = all_bandwidths.size() % 2 == 0 ? 
            (all_bandwidths[mid-1] + all_bandwidths[mid]) / 2.0 : 
            all_bandwidths[mid];
    }

    // Minimum expected bandwidth threshold - dynamic based on connection count
    // This represents the minimum acceptable quality, not a target to achieve
    // The actual target bitrate is set by the client and unknown to us
    double min_total_bandwidth_kbps = MIN_ACCEPTABLE_TOTAL_BANDWIDTH_KBPS;
    double min_expected_kbits_per_sec = min_total_bandwidth_kbps / bandwidth_info.size();
    
    // Set reasonable bounds: not less than 100 kbps (poor mobile) and not more than 500 kbps
    min_expected_kbits_per_sec = std::max(100.0, std::min(500.0, min_expected_kbits_per_sec));
    
    // Check if all connections have similar performance (within 30% of median)
    bool all_similar = true;
    if (median_kbits_per_sec > 0) {
        for (const auto &bw : all_bandwidths) {
            if (bw < median_kbits_per_sec * 0.7 || bw > median_kbits_per_sec * 1.3) {
                all_similar = false;
                break;
            }
        }
    }
    
    // Log the total and expected bandwidth with new metrics
    spdlog::debug("[Group: {}] Total bandwidth: {:.2f} kbits/s, Max: {:.2f} kbits/s, Median: {:.2f} kbits/s, "
                 "Min expected per conn: {:.2f} kbps, All similar: {}",
                 static_cast<void *>(this), total_kbits_per_sec, max_kbits_per_sec, median_kbits_per_sec, 
                 min_expected_kbits_per_sec, all_similar);

    // Second pass - evaluate each connection against dynamic thresholds
    for (auto &info : bandwidth_info) {
        auto conn = info.conn;
        double bandwidth_kbits_per_sec = info.bandwidth_kbits_per_sec;
        double packet_loss_ratio = info.packet_loss_ratio;

        // Reset error points for the new evaluation period
        conn->stats.error_points = 0;

        // Adaptive bandwidth evaluation strategy
        double expected_kbits_per_sec;
        
        if (all_similar) {
            // All connections are similar - use unified expectation for fair distribution
            // Use 80% of median to allow for normal variations
            expected_kbits_per_sec = median_kbits_per_sec * 0.8;
            
            // But respect the minimum threshold
            expected_kbits_per_sec = std::max(expected_kbits_per_sec, min_expected_kbits_per_sec);
            
            spdlog::trace("[{}:{}] Using median-based expectation: {:.2f} kbps",
                         print_addr((struct sockaddr *)&conn->addr), port_no((struct sockaddr *)&conn->addr), expected_kbits_per_sec);
        } else {
            // Mixed quality connections - use adaptive strategy
            
            // Calculate expected based on current performance
            expected_kbits_per_sec = bandwidth_kbits_per_sec * 0.7;
            
            // For good connections: expect them to maintain 70% of their performance
            // For poor connections: use minimum threshold
            expected_kbits_per_sec = std::max(min_expected_kbits_per_sec, 
                                             std::min(expected_kbits_per_sec, max_kbits_per_sec));
            
            spdlog::trace("[{}:{}] Using adaptive expectation: {:.2f} kbps",
                         print_addr((struct sockaddr *)&conn->addr), port_no((struct sockaddr *)&conn->addr), expected_kbits_per_sec);
        }

        // Dynamic bandwidth evaluation based on expected bandwidth
        if (bandwidth_kbits_per_sec < expected_kbits_per_sec * 0.3) {
            // Significantly underperforming - high penalty
            conn->stats.error_points += 40;
        } else if (bandwidth_kbits_per_sec < expected_kbits_per_sec * 0.5) {
            // Moderately underperforming
            conn->stats.error_points += 25;
        } else if (bandwidth_kbits_per_sec < expected_kbits_per_sec * 0.7) {
            // Slightly underperforming
            conn->stats.error_points += 15;
        } else if (bandwidth_kbits_per_sec < expected_kbits_per_sec * 0.85) {
            // Marginally below expected - minimal penalty
            conn->stats.error_points += 5;
        }
        // Connections performing at 85%+ of expected bandwidth get no penalty

        // Packet loss evaluation
            if (packet_loss_ratio > 0.20) { // > 20% loss
                conn->stats.error_points += 40;
            } else if (packet_loss_ratio > 0.10) { // > 10% loss
                conn->stats.error_points += 20;
            } else if (packet_loss_ratio > 0.05) { // > 5% loss
                conn->stats.error_points += 10;
            } else if (packet_loss_ratio > 0.01) { // > 1% loss
                conn->stats.error_points += 5;
            }

        // Reset NAK count
        conn->stats.nack_count = 0;

        spdlog::trace("[{}:{}] [Group: {}] Connection stats: BW: {:.2f} kbits/s ({:.2f}% of expected), Loss: {:.2f}%, Error points: {}",
                print_addr((struct sockaddr *)&conn->addr), port_no((struct sockaddr *)&conn->addr), static_cast<void *>(this),
                bandwidth_kbits_per_sec, (bandwidth_kbits_per_sec / expected_kbits_per_sec) * 100, packet_loss_ratio * 100,
                conn->stats.error_points);
    }
    
    // Adjust connection weights based on error points
    adjust_connection_weights();
    
    last_quality_eval = current_time;
}

void srtla_conn_group::adjust_connection_weights() {
    if (conns.empty())
        return;
        
    bool any_change = false;
    
    // Log current state before adjustment
    spdlog::debug("[Group: {}] Evaluating weights and throttle factors for {} connections", 
                 static_cast<void *>(this), conns.size());
    
    // First pass: Calculate weights and find best performing connection
    uint8_t max_weight = 0;
    int active_conns = 0;
    
    // Adjust weights based on error points
    for (auto &conn : conns) {
        uint8_t old_weight = conn->stats.weight_percent;
        uint8_t new_weight;
        
        // Weight adjustment based on error points
        if (conn->stats.error_points >= 40) {
            new_weight = WEIGHT_CRITICAL;
        } else if (conn->stats.error_points >= 25) {
            new_weight = WEIGHT_POOR;
        } else if (conn->stats.error_points >= 15) {
            new_weight = WEIGHT_FAIR;
        } else if (conn->stats.error_points >= 10) {
            new_weight = WEIGHT_DEGRADED;
        } else if (conn->stats.error_points >= 5) {
            new_weight = WEIGHT_EXCELLENT;
        } else {
            new_weight = WEIGHT_FULL;
        }
        
        // Update weight if changed
        if (new_weight != old_weight) {
            conn->stats.weight_percent = new_weight;
            any_change = true;
        }
        
        // Track maximum weight for throttle calculation
        if (!conn_timed_out(conn, time(nullptr))) {
            max_weight = std::max(max_weight, conn->stats.weight_percent);
            active_conns++;
        }
    }
    
    // Second pass: Calculate throttle factors based on weights
    if (load_balancing_enabled && active_conns > 1) {
        for (auto &conn : conns) {
            double old_throttle = conn->stats.ack_throttle_factor;
            double new_throttle;
            
            // Calculate throttle based on both absolute and relative quality
            // This naturally handles all cases:
            // - Good connections (high absolute weight) get high throttle
            // - Best connections (relative = 1.0) are limited only by absolute quality
            // - Poor connections get limited even if they're the "best" available
            
            double absolute_quality = static_cast<double>(conn->stats.weight_percent) / WEIGHT_FULL;
            double relative_quality = static_cast<double>(conn->stats.weight_percent) / max_weight;
            
            // Use the lower of absolute or relative quality
            // This ensures poor connections never get full rate
            new_throttle = std::min(absolute_quality, relative_quality);

            // Note: WEIGHT_CRITICAL (e.g. 10%) and MIN_ACK_RATE (e.g. 20%) serve different purposes:
            // - WEIGHT_CRITICAL: How bad the connection is (quality assessment)
            // - MIN_ACK_RATE: Minimum ACKs to keep connection alive (operational limit)
            // This separation allows critical connections to be marked as 10% quality
            // while still receiving 20% ACKs for monitoring and recovery potential
            new_throttle = std::max(MIN_ACK_RATE, new_throttle);
            
            spdlog::debug("[{}:{}] Throttle calculation: weight={}, max_weight={}, "
                         "absolute={:.2f}, relative={:.2f}, new_throttle={:.2f}, old_throttle={:.2f}",
                         print_addr((struct sockaddr *)&conn->addr), port_no((struct sockaddr *)&conn->addr), 
                         conn->stats.weight_percent, max_weight,
                         absolute_quality, relative_quality, new_throttle, old_throttle);
            
            // Update throttle factor only if changed
            if (std::abs(old_throttle - new_throttle) > 0.01) {
                conn->stats.ack_throttle_factor = new_throttle;
                any_change = true;
                spdlog::debug("[{}:{}] Throttle factor updated: {:.2f} -> {:.2f}",
                             print_addr((struct sockaddr *)&conn->addr), port_no((struct sockaddr *)&conn->addr), 
                             old_throttle, new_throttle);
            }
        }
    } else {
        // Single connection or load balancing disabled - no throttling
        for (auto &conn : conns) {
            if (conn->stats.ack_throttle_factor != 1.0) {
                conn->stats.ack_throttle_factor = 1.0;
                any_change = true;
            }
        }
    }
    
    // Log all changes in one comprehensive summary
    if (any_change) {
        spdlog::info("[Group: {}] Connection parameters adjusted:", static_cast<void *>(this));
        
        for (auto &conn : conns) {
            spdlog::info("  [{}:{}] Weight: {}%, Throttle: {:.2f}, Error points: {}, "
                        "Bandwidth: {} bytes, Packets: {}, Loss: {}",
                        print_addr((struct sockaddr *)&conn->addr), port_no((struct sockaddr *)&conn->addr),
                        conn->stats.weight_percent,
                        conn->stats.ack_throttle_factor,
                        conn->stats.error_points,
                        conn->stats.bytes_received,
                        conn->stats.packets_received,
                        conn->stats.packets_lost);
        }
    } else {
        spdlog::debug("[Group: {}] No weight or throttle adjustments needed", static_cast<void *>(this));
    }
}

// Implementation for Problem 1: Connections with Recovery
void send_keepalive(srtla_conn_ptr c, time_t ts) {
    uint16_t pkt = htobe16(SRTLA_TYPE_KEEPALIVE);
    int ret = sendto(srtla_sock, &pkt, sizeof(pkt), 0, (struct sockaddr *)&c->addr, addr_len);
    
    if (ret != sizeof(pkt)) {
        spdlog::error("[{}:{}] Failed to send keepalive packet",
            print_addr((struct sockaddr *)&c->addr), port_no((struct sockaddr *)&c->addr));
    } else {
        spdlog::debug("[{}:{}] Sent keepalive packet",
            print_addr((struct sockaddr *)&c->addr), port_no((struct sockaddr *)&c->addr));
    }
}

bool conn_timed_out(srtla_conn_ptr c, time_t ts) {
  return (c->last_rcvd + CONN_TIMEOUT) < ts;
}

int main(int argc, char **argv) {
  argparse::ArgumentParser args("srtla_rec", VERSION);

  args.add_argument("--srtla_port").help("Port to bind the SRTLA socket to").default_value((uint16_t)5000).scan<'d', uint16_t>();
  args.add_argument("--srt_hostname").help("Hostname of the downstream SRT server").default_value(std::string{"127.0.0.1"});
  args.add_argument("--srt_port").help("Port of the downstream SRT server").default_value((uint16_t)4001).scan<'d', uint16_t>();
  args.add_argument("--log_level").help("Set logging level (trace, debug, info, warn, error, critical)").default_value(std::string{"info"});

  try {
    args.parse_args(argc, argv);
  } catch (const std::runtime_error &err) {
    std::cerr << err.what() << std::endl;
    std::cerr << args;
    std::exit(1);
  }

  uint16_t srtla_port = args.get<uint16_t>("--srtla_port");
  std::string srt_hostname = args.get<std::string>("--srt_hostname");
  std::string srt_port = std::to_string(args.get<uint16_t>("--srt_port"));
  std::string log_level = args.get<std::string>("--log_level");

  // Set log level based on the provided argument
  if (log_level == "trace") {
    spdlog::set_level(spdlog::level::trace);
  } else if (log_level == "debug") {
    spdlog::set_level(spdlog::level::debug);
  } else if (log_level == "info") {
    spdlog::set_level(spdlog::level::info);
  } else if (log_level == "warn") {
    spdlog::set_level(spdlog::level::warn);
  } else if (log_level == "error") {
    spdlog::set_level(spdlog::level::err);
  } else if (log_level == "critical") {
    spdlog::set_level(spdlog::level::critical);
  } else {
    spdlog::warn("Invalid log level '{}' specified, using 'info' as default", log_level);
    spdlog::set_level(spdlog::level::info);
  }

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
  srtla_sock = socket(AF_INET6, SOCK_DGRAM, 0);
  if (srtla_sock < 0) {
    spdlog::critical("SRTLA socket creation failed");
    exit(EXIT_FAILURE);
  }

  // Disable IPV6_V6ONLY
  int v6only = 0;
  ret = setsockopt(srtla_sock, IPPROTO_IPV6, IPV6_V6ONLY, &v6only,
                   sizeof(v6only));
  if (ret < 0) {
    spdlog::critical("Failed to set IPV6_V6ONLY option");
    exit(EXIT_FAILURE);
  }

  // Set receive buffer size for srtla_sock
  int bufsize = RECV_BUF_SIZE;
  ret = setsockopt(srtla_sock, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
  if (ret != 0) {
    spdlog::error("failed to set receive buffer size ({})", bufsize);
    exit(EXIT_FAILURE);
  }

  // Set send buffer size for srtla_sock
  bufsize = SEND_BUF_SIZE;
  ret = setsockopt(srtla_sock, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
  if (ret != 0) {
    spdlog::error("failed to set send buffer size ({})", bufsize);
    exit(EXIT_FAILURE);
  }

  // Set srtla_sock to non-blocking
  int flags = fcntl(srtla_sock, F_GETFL, 0);
  if (flags == -1 || fcntl(srtla_sock, F_SETFL, flags | O_NONBLOCK) == -1) {
    spdlog::error("failed to set srtla_sock non-blocking");
    exit(EXIT_FAILURE);
  }

  struct sockaddr_in6 listen_addr = {};
  listen_addr.sin6_family = AF_INET6;
  listen_addr.sin6_addr = in6addr_any;
  // Use the original srtla_port
  listen_addr.sin6_port = htons(srtla_port);
  ret = bind(srtla_sock, (const struct sockaddr *)&listen_addr,
             sizeof(listen_addr));
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

  while (true) {
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
  }
}
