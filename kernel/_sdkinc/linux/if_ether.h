#ifndef KRX_LINUX_IF_ETHER_H
#define KRX_LINUX_IF_ETHER_H

#include <stdint.h>

struct ethhdr {
	uint8_t h_dest[6];
	uint8_t h_source[6];
	uint16_t h_proto;
};

#define ETH_P_IP 0x800 /* IP packet type */

#endif /* KRX_LINUX_IF_ETHER_H */
