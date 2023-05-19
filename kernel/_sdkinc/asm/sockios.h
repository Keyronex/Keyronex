#ifndef KRX_ASM_SOCKIOS_H
#define KRX_ASM_SOCKIOS_H

#include <stdint.h>

#define SIOCGIFADDR 0x8915    /* get IP address  */
#define SIOCGSIFADDR 0x8916    /* set IP address  */
#define SIOCGIFNETMASK 0x891b /* get netmask  */
#define SIOCSIFNETMASK 0x891c /* set netmask */
#define SIOCGIFHWADDR 0x8927  /* get hardware addr */
#define SIOCGIFGATEWAY 0x89c0 /* get default gateway */
#define SIOCSIFGATEWAY 0x89c1 /* set default gateway */
#define SIOCGIFSTATS 0x89d0   /* get interface statistics */

struct krx_if_statistics {
	uint64_t rx_packets, rx_bytes;
	uint64_t rx_errors, rx_drops;
	uint64_t tx_packets, tx_bytes;
	uint64_t tx_errors, tx_drops;
};

#endif /* KRX_ASM_SOCKIOS_H */
