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

#include <endian.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#include "common.h"

#define ADDR_BUF_SZ 50
char _global_addr_buf[ADDR_BUF_SZ];
const char *print_addr(struct sockaddr *addr) {
  struct sockaddr_in *ain = (struct sockaddr_in *)addr;
  return inet_ntop(ain->sin_family, &ain->sin_addr, _global_addr_buf, ADDR_BUF_SZ);
}

int port_no(struct sockaddr *addr) {
  struct sockaddr_in *ain = (struct sockaddr_in *)addr;
  return ntohs(ain->sin_port);
}

int parse_ip(struct sockaddr_in *addr, char *ip_str) {
  in_addr_t ip = inet_addr(ip_str);
  if (ip == -1) return -1;

  memset(addr, 0, sizeof(*addr));
  addr->sin_family = AF_INET;
  addr->sin_addr.s_addr = ip;

  return 0;
}

int parse_port(char *port_str) {
  int port = strtol(port_str, NULL, 10);
  if (port <= 0 || port > 65535) return -2;
  return port;
}

int get_seconds(time_t *s) {
  struct timespec ts;
  int ret = clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
  if (ret != 0) return -1;
  *s = ts.tv_sec;
  return 0;
}

int get_ms(uint64_t *ms) {
  struct timespec ts;
  int ret = clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
  if (ret != 0) return -1;
  *ms = ((uint64_t)(ts.tv_sec)) * 1000 + ((uint64_t)(ts.tv_nsec)) / 1000 / 1000;

  return 0;
}

int32_t get_srt_sn(void *pkt, int n) {
  if (n < 4) return -1;

  uint32_t sn = be32toh(*((uint32_t *)pkt));
  if ((sn & (1 << 31)) == 0) {
    return (int32_t)sn;
  }

  return -1;
}

uint16_t get_srt_type(void *pkt, int n) {
  if (n < 2) return 0;
  return be16toh(*((uint16_t *)pkt));
}

int is_srt_ack(void *pkt, int n) {
  return get_srt_type(pkt, n) == SRT_TYPE_ACK;
}

int is_srtla_keepalive(void *pkt, int n) {
  return get_srt_type(pkt, n) == SRTLA_TYPE_KEEPALIVE;
}

int is_srtla_reg1(void *pkt, int len) {
  if (len != SRTLA_TYPE_REG1_LEN) return 0;
  return get_srt_type(pkt, len) == SRTLA_TYPE_REG1;
}

int is_srtla_reg2(void *pkt, int len) {
  if (len != SRTLA_TYPE_REG2_LEN) return 0;
  return get_srt_type(pkt, len) == SRTLA_TYPE_REG2;
}

int is_srtla_reg3(void *pkt, int len) {
  if (len != SRTLA_TYPE_REG3_LEN) return 0;
  return get_srt_type(pkt, len) == SRTLA_TYPE_REG3;
}

int parse_keepalive_conn_info(const uint8_t *buf, int len, connection_info_t *info) {
  if (len < SRTLA_KEEPALIVE_EXT_LEN) return 0;
  
  uint16_t packet_type = (buf[0] << 8) | buf[1];
  if (packet_type != SRTLA_TYPE_KEEPALIVE) return 0;
  
  // Check magic number at bytes 10-11
  uint16_t magic = (buf[10] << 8) | buf[11];
  if (magic != SRTLA_KEEPALIVE_MAGIC) return 0;
  
  // Check version at bytes 12-13
  uint16_t version = (buf[12] << 8) | buf[13];
  if (version != SRTLA_KEEPALIVE_EXT_VERSION) return 0;
  
  // Parse connection info (all big-endian)
  info->conn_id = ((uint32_t)buf[14] << 24) | ((uint32_t)buf[15] << 16) | ((uint32_t)buf[16] << 8) | buf[17];
  info->window = (int32_t)(((uint32_t)buf[18] << 24) | ((uint32_t)buf[19] << 16) | ((uint32_t)buf[20] << 8) | buf[21]);
  info->in_flight = (int32_t)(((uint32_t)buf[22] << 24) | ((uint32_t)buf[23] << 16) | ((uint32_t)buf[24] << 8) | buf[25]);
  info->rtt_us = ((uint64_t)buf[26] << 56) | ((uint64_t)buf[27] << 48) | 
                 ((uint64_t)buf[28] << 40) | ((uint64_t)buf[29] << 32) |
                 ((uint64_t)buf[30] << 24) | ((uint64_t)buf[31] << 16) |
                 ((uint64_t)buf[32] << 8)  | (uint64_t)buf[33];
  info->nak_count = ((uint32_t)buf[34] << 24) | ((uint32_t)buf[35] << 16) | ((uint32_t)buf[36] << 8) | buf[37];
  info->bitrate_bytes_per_sec = ((uint32_t)buf[38] << 24) | ((uint32_t)buf[39] << 16) | ((uint32_t)buf[40] << 8) | buf[41];
  
  return 1;
}
