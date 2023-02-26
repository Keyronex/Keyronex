/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Fri Feb 24 2023.
 */

#include "kdk/devmgr.h"
#include "kdk/kerndefs.h"
#include "kdk/libkern.h"
#include "kdk/vm.h"
#include "uuid.h"

#include "volmgr.hh"

static int sequence_num = 0;

struct gpt_header {
	char signature[8];
	uint32_t revision;
	uint32_t size;
	uint32_t cksumHeader;
	uint32_t reserved;
	uint64_t lbaHeader;
	uint64_t lbaAltHeader;
	uint64_t lbaFirstUsable;
	uint64_t lbaLastUsable;
	uuid_t guid;
	uint64_t lbaEntryArrayStart;
	uint32_t nEntries;
	uint32_t sizEntry;
	uint32_t cksumPart;
	uint32_t reserved2;
};

struct gpt_entry {
	uuid_t type;
	uuid_t identifier;
	uint64_t lbaStart;
	uint64_t lbaEnd;
	uint64_t attributes;
	uint16_t name[36]; /* UCS-2 */
};

void
VolumeManager::enumerateGPTPartitions()
{
	vm_mdl_t *mdl;
	iop_t *iop;
	struct gpt_header *header;
	void *addr;
	size_t nEntries, sizEntry;
	uint64_t entryArrayStart, entryArrayEnd;
	int r;

	mdl = vm_mdl_buffer_alloc(1);
	iop = iop_new_read(provider, mdl, info.block_size, info.block_size);
	r = iop_send_sync(iop);

	kassert(r == kIOPRetCompleted);

	vm_mdl_map(mdl, &addr);
	header = (gpt_header *)addr;

	if (memcmp(header->signature, "EFI PART", 8) != 0) {
		DKDevLog(this, "Not a GPT disk\n");
		return;
	}

	DKDevLog(this, "GUID partition scheme found\n");

	nEntries = header->nEntries;
	sizEntry = header->sizEntry;
	entryArrayStart = header->lbaEntryArrayStart * info.block_size;
	entryArrayEnd = entryArrayStart + nEntries * sizEntry;

	/* really just needs be a power of 2 for the below stupid logic */
	kassert(sizEntry == sizeof(gpt_entry));

	for (auto part = entryArrayStart; part < entryArrayEnd;
	     part += PGSIZE) {
		size_t partSize = MIN2(entryArrayEnd - part, PGSIZE);
		size_t istart;

		iop = iop_new_read(provider, mdl, partSize, part);
		r = iop_send_sync(iop);
		kassert(r == kIOPRetCompleted);

		istart = (part - entryArrayStart);

		for (size_t i = istart; i < istart + partSize;
		     i += sizeof(gpt_entry)) {
			struct gpt_entry *ent = (gpt_entry *)((char *)addr +
			    (i - istart));
			char parttype[UUID_STRING_LENGTH + 1] = { 0 };
			char partname[37];

			if (uuid_is_null(ent->type))
				continue;

			/** TODO(low): handle UTF-16 properly */
			for (int i = 0; i < 36; i++)
				partname[i] = ent->name[i] > 127 ? '?' :
								   ent->name[i];
			uuid_unparse(ent->type, parttype);

			DKDevLog(this, "Found (i = %ld) %s (type %s)\n", i,
			    partname, parttype);
		}
	}
}

VolumeManager::VolumeManager(device_t *provider, struct volmgr_disk_info &info)
    : info(info)
{
	kmem_asprintf(&objhdr.name, "volmgr%d", sequence_num++);
	attach(provider);

	enumerateGPTPartitions();
}
