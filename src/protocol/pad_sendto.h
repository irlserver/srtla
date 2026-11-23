#pragma once
#include <cstring>
#include <sys/socket.h>
/* Pad small sendto() to 32 bytes to avoid carrier NAT drops on 2-byte packets */
static inline int pad_sendto(int sock, const void *buf, size_t len,
                             int flags, const struct sockaddr *addr, socklen_t alen) {
  unsigned char padded[32];
  if (len >= 32) return sendto(sock, buf, len, flags, addr, alen);
  memset(padded, 0, 32);
  memcpy(padded, buf, len);
  int ret = sendto(sock, padded, 32, flags, addr, alen);
  return (ret == 32) ? (int)len : ret;
}

// SRTLA ACKs use their datagram length to encode the sequence count.
// Carrier padding would add sequence numbers, so send their exact length.
static inline int send_srtla_ack(int sock, const void *buf, size_t len,
                                 const struct sockaddr *addr, socklen_t alen) {
  return sendto(sock, buf, len, 0, addr, alen);
}
