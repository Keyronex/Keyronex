#ifndef KRX_VM_VMP_DYNAMICS_H
#define KRX_VM_VMP_DYNAMICS_H

#include "kdk/libkern.h"
#include "vmp.h"

extern kevent_t vmp_writer_event, vmp_balance_set_scheduler_event,
    vmp_page_availability_event;

/* below this, allocations are made by stealing a page */
static inline bool
vmp_free_pages_low(void)
{
	return vmstat.nfree < 64;
}

/* below this, all nonessential allocations are disallowed */
static inline bool
vmp_avail_pages_very_low(void)
{
	return (vmstat.nfree + vmstat.nstandby) < 96;
}

/* below this, balance set scheduler runs enthusiastically */
static inline bool
vmp_avail_pages_low(void)
{
	return (vmstat.nfree + vmstat.nstandby) < 168;
}

/* below this, modified page writer runs enthusiastically*/
static inline bool
vmp_avail_pages_fairly_low(void)
{
	return (vmstat.nfree + vmstat.nstandby) <
	    MAX2(384, vmstat.ntotal / 256);
}

static inline bool
vmp_writer_should_run(void)
{
	if (vmstat.nmodified >= 16 && vmp_avail_pages_fairly_low())
		return true;
	else if (vmstat.nmodified >= vmstat.ntotal / 128)
		return true;
	else
		return false;
}

static inline void
vmp_update_events(void)
{
	if (vmp_avail_pages_low())
		ke_event_signal(&vmp_balance_set_scheduler_event);
	if (vmp_writer_should_run())
		ke_event_signal(&vmp_writer_event);
}

#endif /* KRX_VM_VMP_DYNAMICS_H */
