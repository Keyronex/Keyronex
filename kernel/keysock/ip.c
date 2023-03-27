/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Sun Mar 26 2023.
 */

#include "kdk/libkern.h"
#include "sockfs.h"

int
addr_unpack_ip(const struct sockaddr *nam, socklen_t namlen,
    krx_out ip_addr_t *ip_out, krx_out uint16_t *port_out)
{
	struct sockaddr_in *sin = (struct sockaddr_in *)nam;
	inet_addr_to_ip4addr(ip_2_ip4(ip_out), &(sin->sin_addr));
	*port_out = lwip_ntohs(sin->sin_port);
	return 0;
}

int
addr_pack_ip(krx_out struct sockaddr *addr, krx_inout socklen_t *addrlen,
    ip_addr_t *ip, uint16_t port)
{
	struct sockaddr_in *sin = (struct sockaddr_in *)addr;

	if (*addrlen < sizeof(struct sockaddr_in))
		return -EINVAL;

	sin->sin_family = AF_INET;
	sin->sin_port = lwip_htons((port));
	inet_addr_from_ip4addr(&sin->sin_addr, ip);
	memset(sin->sin_zero, 0, sizeof(sin->sin_zero));
	*addrlen = sizeof(struct sockaddr_in);

	return 0;
}