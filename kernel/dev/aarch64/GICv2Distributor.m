/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Sun Jul 28 2024.
 */

#include "../../platform/aarch64-virt/gic.h"
#include "dev/aarch64/GICv2Distributor.h"

@implementation GICv2Distributor

+ (int)handleGSI:(uint32_t)gsi
	withHandler:(intr_handler_t)handler
	   argument:(void *)arg
      isLowPolarity:(bool)lopol
    isEdgeTriggered:(bool)isEdgeTriggered
	 atPriority:(ipl_t)prio
	      entry:(struct intr_entry *)entry
{
	md_intr_register("gsi", gsi, prio, handler, arg, !isEdgeTriggered, entry);

	gengic_dist_setedge(gsi, isEdgeTriggered);
	gengic_dist_settarget(gsi, (1 << ncpus) - 1);
	gengic_dist_setenabled(gsi);
}

@end
