#ifndef KRX_INCLUDE_LWIPOPTS_H
#define KRX_INCLUDE_LWIPOPTS_H

#include <abi-bits/errno.h>

#include "kdk/nanokern.h"
#include "kdk/libkern.h"

#undef errno
#define errno curcpu()->curthread->errno

#if 1
#define LWIP_DEBUG 1
#define MEMP_DEBUG LWIP_DBG_ON
#define TCP_DEBUG                       LWIP_DBG_ON
#define TCP_INPUT_DEBUG LWIP_DBG_ON
#define TCP_OUTPUT_DEBUG LWIP_DBG_ON
#define ETHARP_DEBUG                    LWIP_DBG_ON
#define PBUF_DEBUG                      LWIP_DBG_ON
#define IP_DEBUG                        LWIP_DBG_ON
#define TCPIP_DEBUG                     LWIP_DBG_ON
#define DHCP_DEBUG                      LWIP_DBG_ON
#define UDP_DEBUG                       LWIP_DBG_ON
#endif

/*! We chain pbufs in ethernet device output queues. */
#define LWIP_PBUF_CUSTOM_DATA STAILQ_ENTRY(pbuf) stailq_entry;

#define LWIP_TIMERS 1
#define LWIP_NETIF_API 1
#define LWIP_SOCKET 0
#define LWIP_RAW 1
#define LWIP_STATS 1
#define MIB2_STATS 1


#define LWIP_CHECKSUM_ON_COPY 1

#define TCPIP_MBOX_SIZE 256

#if 0
#define MEM_SIZE 1024  * 1024
#define TCP_MSS 1460
#define TCP_WND 4096 * 16 - 1
#define MEMP_NUM_PBUF 256
#define MEMP_NUM_TCPIP_MSG_INPKT 64
#define MEMP_NUM_TCPIP_MSG_API 64
#define PBUF_POOL_SIZE 64
#define MEMP_USE_CUSTOM_POOLS 1
#define MEM_USE_POOLS 1
#endif


#endif /* KRX_INCLUDE_LWIPOPTS_H */
