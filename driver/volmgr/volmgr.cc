/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Fri Feb 24 2023.
 */

#include "kdk/devmgr.h"
#include "kdk/kerndefs.h"
#include "uuid.h"
#include "volmgr.hh"

static int sequence_num = 0;

struct gpt_header {
	char	 signature[8];
	uint32_t revision;
	uint32_t size;
	uint32_t cksumHeader;
	uint32_t reserved;
	uint64_t lbaHeader;
	uint64_t lbaAltHeader;
	uint64_t lbaFirstUsable;
	uint64_t lbaLastUsable;
	uuid_t	 guid;
	uint64_t lbaEntryArrayStart;
	uint32_t nEntries;
	uint32_t sizEntry;
	uint32_t cksumPart;
	uint32_t reserved2;
};

struct gpt_entry {
	uuid_t	 type;
	uuid_t	 identifier;
	uint64_t lbaStart;
	uint64_t lbaEnd;
	uint64_t attributes;
	uint16_t name[36]; /* UCS-2 */
};

VolumeManager::VolumeManager(device_t *provider)
 {
	vm_mdl_t *mdl;
	io_bsize_t bsize;
	iop_t *iop;
	int r;

	kmem_asprintf(&objhdr.name, "volmgr%d", sequence_num++);
	attach(provider);
 
	mdl = vm_mdl_buffer_alloc(1);
	iop = iop_new_ioctl(provider, kIOCTLDiskBlockSize, mdl, sizeof(bsize));
	r = iop_send_sync(iop);

	kdprintf("R: %d, bsize: %lu\n", r, *(io_bsize_t*)P2V(mdl->pages[0]->address));

 }
