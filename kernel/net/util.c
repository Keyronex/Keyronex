#include <netinet/in.h>

#include "util.h"

int
addr_unpack_ip(const struct sockaddr_storage *nam, ip_addr_t *ip_out,
    uint16_t *port_out)
{
	if (nam->ss_family == AF_INET6) {
		kfatal("handle inet6!\n");
	} else if (nam->ss_family == AF_INET) {
		struct sockaddr_in *sin = (struct sockaddr_in *)nam;
		ip_out->type = IPADDR_TYPE_V4;
		ip4_addr_set_u32(&ip_out->u_addr.ip4, sin->sin_addr.s_addr);
		*port_out = lwip_ntohs(sin->sin_port);
	} else {
		return -EAFNOSUPPORT;
	}

	return 0;
}

void
addr_pack_ip(struct sockaddr_storage *addr, ip_addr_t *ip, uint16_t port)
{
	if (IP_IS_V6(ip)) {
		kfatal("handle inet6!\n");
	} else {
		struct sockaddr_in *sin = (struct sockaddr_in *)addr;
		sin->sin_family = AF_INET;
		sin->sin_port = lwip_htons((port));
		sin->sin_addr.s_addr = ip4_addr_get_u32(&ip->u_addr.ip4);
		memset(sin->sin_zero, 0, sizeof(sin->sin_zero));
	}
}
