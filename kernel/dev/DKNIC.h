#ifndef KRX_DEV_DKNIC_H
#define KRX_DEV_DKNIC_H

#ifdef __OBJC__
#include "ddk/DKDevice.h"
#endif
#include "lwip/netif.h"
#include "lwip/pbuf.h"

typedef uint8_t dk_mac_address_t[6];
#define MAC_FMT "%02x:%02x:%02x:%02x:%02x:%02x"
#define MAC_ARGS(MAC) MAC[0], MAC[1], MAC[2], MAC[3], MAC[4], MAC[5]

struct pbuf_rx {
	struct pbuf_custom pbuf;
	/*! identifier for resource of NIC used, to help free it later */
	uint32_t hdr_desc_id;
	/* whether this is being freed from within processBuffers() */
	bool locked;
	/* linkage in packet ingress queue */
	TAILQ_ENTRY(pbuf_rx) queue_entry;
	struct netif *netif;
};

#ifdef __OBJC__
@interface DKNIC : DKDevice {
	BOOL m_kdbAttached;

	size_t m_queueLength;
	size_t m_rxBufSize;

	dk_mac_address_t m_mac;
	struct netif m_netif;

	/*! preallocated packet buffers for received data */
	struct pbuf_rx *m_rxPbufs;
	/*! received packets queued for draining in - drainReceivedPackets. */
	struct pbuf_rx *m_rxedPackets;
}

/*! DKNIC: initialise with given queue length (for both RX and TX queues) */
- (void)setupForQueueLength:(size_t)size rxBufSize:(size_t)rxBufSize;
/*! DKNIC: set up netif (call after MAC address known & able to transmit) */
- (void)setupNetif;
/*! DKNIC: set link up/down */
- (void)setLinkUp:(BOOL)up speed:(size_t)mbits fullDuplex:(BOOL)duplex;
/*! DKNIC: submit packet data for eventual processing. YES = break processing */
- (void)queueReceivedDataForProcessing:(void *)data
				length:(size_t)length
				    id:(uint16_t)id;

/*
 * subclass methods
 */
/*! subclass: submit packet for immediate or future transmission */
- (void)submitPacket:(struct pbuf *)pkt;
/*! subclass: processing of an index in the RX queue completed; resource free */
- (void)completeProcessingOfRxIndex:(size_t)index locked:(BOOL)isLocked;

/*! subclass: poll for a received packet on behalf of kernel debugger */
- (BOOL)debuggerPoll;
/*! subclass: transmit a packet on behalf of kernel debugger */
- (void)debuggerTransmit:(struct pbuf *)pbuf;

@end
#endif

#endif /* KRX_DEV_DKNIC_H */
