/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Sun Mar 26 2023.
 */

#include "sockfs.h"

int
addr_unpack_ip(const struct sockaddr *nam, socklen_t namlen, ip_addr_t *ip_out,
    uint16_t *port_out)
{
	struct sockaddr_in *sin = (struct sockaddr_in *)nam;
	inet_addr_to_ip4addr(ip_2_ip4(ip_out), &(sin->sin_addr));
	*port_out = lwip_ntohs(sin->sin_port);
	return 0;
}