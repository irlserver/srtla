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

#include <stdint.h>
#include <time.h>

#define MTU 1500

#define SRT_TYPE_HANDSHAKE   0x8000
#define SRT_TYPE_ACK         0x8002
#define SRT_TYPE_NAK         0x8003
#define SRT_TYPE_SHUTDOWN    0x8005

#define SRTLA_TYPE_KEEPALIVE 0x9000
#define SRTLA_TYPE_ACK       0x9100
#define SRTLA_TYPE_REG1      0x9200
#define SRTLA_TYPE_REG2      0x9201
#define SRTLA_TYPE_REG3      0x9202
#define SRTLA_TYPE_REG_ERR   0x9210
#define SRTLA_TYPE_REG_NGP   0x9211
#define SRTLA_TYPE_REG_NAK   0x9212

// irlserver SRTLA Protocol Extensions (0x9F00-0x9FFF range)
// Extension negotiation packets
#define SRTLA_EXT_HELLO         0x9FF0
#define SRTLA_EXT_ACK           0x9FF1

// Extension capability flags (bitmask)
#define SRTLA_EXT_CAP_CONN_INFO 0x00000001

// Connection info telemetry packet
#define SRTLA_EXT_CONN_INFO     0x9F00

// Extension protocol version
#define SRTLA_EXT_VERSION       0x0001

// Legacy IRLToolkit extensions (kept for compatibility)
#define SRTLA_EXT_IRLTK_CIP_REQ 0xA000
#define SRTLA_EXT_IRLTK_CIP_RES 0xA001

#define SRT_MIN_LEN          16

#define SRTLA_ID_LEN         256
#define SRTLA_TYPE_REG1_LEN  (2 + (SRTLA_ID_LEN))
#define SRTLA_TYPE_REG2_LEN  (2 + (SRTLA_ID_LEN))
#define SRTLA_TYPE_REG3_LEN  2

// Extension packet lengths
#define SRTLA_EXT_HELLO_LEN     10
#define SRTLA_EXT_ACK_LEN       10
#define SRTLA_EXT_CONN_INFO_LEN 32

// Legacy extension lengths
#define SRTLA_EXT_IRLTK_CIP_REQ_LEN 2
#define SRTLA_EXT_IRLTK_CIP_RES_LEN (2 + sizeof(srtla_pkt_irltk_cip_res))

#define SEND_BUF_SIZE (100 * 1024 * 1024)
#define RECV_BUF_SIZE (100 * 1024 * 1024)

typedef struct __attribute__((__packed__)) {
  uint16_t type;
  uint16_t subtype;
  uint32_t info;
  uint32_t timestamp;
  uint32_t dest_id;
} srt_header_t;

typedef struct __attribute__((__packed__)) {
  srt_header_t header;
  uint32_t version;
  uint16_t enc_field;
  uint16_t ext_field;
  uint32_t initial_seq;
  uint32_t mtu;
  uint32_t mfw;
  uint32_t handshake_type;
  uint32_t source_id;
  uint32_t syn_cookie;
  char     peer_ip[16];
} srt_handshake_t;

typedef struct __attribute__((__packed__)) {
  uint8_t address_family;
  uint8_t address[16];
} srtla_ext_irltk_cip_res;

int get_seconds(time_t *s);
int get_ms(uint64_t *ms);

const char *print_addr(struct sockaddr *addr);
int port_no(struct sockaddr *addr);
int parse_ip(struct sockaddr_in *addr, char *ip_str);
int parse_port(char *port_str);

int32_t get_srt_sn(void *pkt, int n);
uint16_t get_srt_type(void *pkt, int n);
int is_srt_ack(void *pkt, int n);
int is_srt_shutdown(void *pkt, int n);

int is_srtla_keepalive(void *pkt, int len);
int is_srtla_reg1(void *pkt, int len);
int is_srtla_reg2(void *pkt, int len);
int is_srtla_reg3(void *pkt, int len);

// Extension packet detection functions
int is_srtla_ext_hello(void *pkt, int len);
int is_srtla_ext_ack(void *pkt, int len);
int is_srtla_ext_conn_info(void *pkt, int len);
