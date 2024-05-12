#include "ddk/DKDevice.h"

#include "9pfs.h"
#include "dev/virtio/VirtIO9pPort.h"
#include "fs/9p/9p_buf.h"
#include "kdk/kmem.h"
#include "kdk/object.h"

#define PROVIDER ((VirtIO9pPort *)m_provider)

static int counter =0;

@implementation NinepFS

-(int)negotiateVersion {
		iop_t *iop;
	struct ninep_buf *buf_in, *buf_out;

	buf_in = ninep_buf_alloc("dS8");
	buf_out = ninep_buf_alloc("dS16");

	buf_in->data->tag = to_leu16(-1);
	buf_in->data->kind = k9pVersion;
	ninep_buf_addu32(buf_in, 8288);
	ninep_buf_addstr(buf_in, k9pVersion2000L);
	ninep_buf_close(buf_in);

	iop = iop_new_9p(PROVIDER, buf_in, buf_out, NULL);
	iop_send_sync(iop);

	switch (buf_out->data->kind) {
	case k9pVersion + 1: {
		char *ver;
		uint32_t msize;

		ninep_buf_getu32(buf_out, &msize);
		ninep_buf_getstr(buf_out, &ver);

		DKDevLog(self, "Negotiated 9p version %s, message size %d\n",
		    ver, msize);
		break;
	}

	default: {
		kfatal("9p failure\n");
	}
	}

	return 0;
}

- (instancetype) initWithProvider: (VirtIO9pPort*) provider
{
	self = [super initWithProvider:provider];

	kmem_asprintf(obj_name_ptr(self), "virtio-9p-%u", counter++);
	[self negotiateVersion];
	kfatal("9p port\n");
}

+ (BOOL)probeWithProvider: (VirtIO9pPort*) provider
{
	return [[self alloc] initWithProvider:provider] != nil;
}

@end
