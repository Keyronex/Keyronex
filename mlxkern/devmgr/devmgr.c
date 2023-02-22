/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Wed Feb 22 2023.
 */

#include "kdk/devmgr.h"
#include "kdk/kernel.h"

static kspinlock_t devmgr_lock = KSPINLOCK_INITIALISER;

void
dev_attach(device_t *consumer, device_t *provider)
{
	TAILQ_INIT(&consumer->consumers);
	consumer->provider = provider;

	if (!provider) {
		consumer->provider = NULL;
		consumer->stack_depth = 1;
	} else {
		ipl_t ipl;
		consumer->provider = provider;
		ipl = ke_spinlock_acquire(&devmgr_lock);
		TAILQ_INSERT_TAIL(&provider->consumers, consumer,
		    consumers_link);
		consumer->stack_depth = provider->stack_depth + 1;
		ke_spinlock_release(&devmgr_lock, ipl);
	}
}