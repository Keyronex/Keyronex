#ifndef KRX_AARCH64_GICV2DISTRIBUTOR_H
#define KRX_AARCH64_GICV2DISTRIBUTOR_H

#include "ddk/DKDevice.h"

@interface GICv2Distributor: DKDevice

+ (int)handleSource:(dk_interrupt_source_t *)source
	withHandler:(intr_handler_t)handler
	   argument:(void *)arg
	 atPriority:(ipl_t)prio
	      entry:(struct intr_entry *)entry;
@end

#endif /* KRX_AARCH64_GICV2DISTRIBUTOR_H */
