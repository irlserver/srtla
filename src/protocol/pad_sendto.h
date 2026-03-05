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
