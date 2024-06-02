#ifndef KRX_NET_ADDR_H
#define KRX_NET_ADDR_H

#include <sys/socket.h>

#include "lwip/ip.h"

void addr_pack_ip(struct sockaddr_storage *addr, ip_addr_t *ip, uint16_t port);
int addr_unpack_ip(const struct sockaddr_storage *nam, ip_addr_t *ip_out,
    uint16_t *port_out);

#endif /* KRX_NET_ADDR_H */
