/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Sun Jul 28 2024.
 */

#if 0
#include "GICv2Distributor.h"
#include "gic.h"

@implementation GICv2Distributor

+ (int)handleSource:(dk_interrupt_source_t *)source
	withHandler:(intr_handler_t)handler
	   argument:(void *)arg
	 atPriority:(ipl_t)prio
	      entry:(struct intr_entry *)entry
{
	uint32_t gsi = source->id;

	md_intr_register("gsi", gsi, prio, handler, arg, !source->edge, entry);

	gengic_dist_setedge(gsi, !source->edge);
	gengic_dist_settarget(gsi, (1 << ncpus) - 1);
	gengic_dist_setenabled(gsi);

	return 0;
}

@end
#endif
