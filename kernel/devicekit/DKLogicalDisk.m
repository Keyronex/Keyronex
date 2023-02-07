/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*
 * Copyright 2022 NetaScale Systems Ltd.
 * All rights reserved.
 */

#include <sys/sysmacros.h>

#include <dev/dev.h>
#include <devicekit/DKDisk.h>
#include <devicekit/DKGPTVolumeManager.h>
#include <kern/kmem.h>
#include <libkern/libkern.h>

#include <errno.h>
#include "md/vm.h"
#include "vm/vm.h"

static int major = -1;
static int minor = 0;

static TAILQ_TYPE_HEAD(ld_tailq,
    DKLogicalDisk) ld_tailq = TAILQ_HEAD_INITIALIZER(ld_tailq);

@implementation DKLogicalDisk

@synthesize underlying = m_underlying;
@synthesize base = m_base;
@synthesize size = m_size;
@synthesize location = m_location;

static int
ld_open(dev_t dev, struct vnode **out, int mode)
{
	DKLogicalDisk *ld;
	TAILQ_FOREACH (ld, &ld_tailq, m_ld_tailq_entry) {
		if (ld->m_minor == minor(dev)) {
			return 0;
		}
	}
	return -ENXIO;
}


static int
ld_read(dev_t dev, void *buf, size_t nbyte, off_t off)
{
	DKLogicalDisk *ld;
	TAILQ_FOREACH (ld, &ld_tailq, m_ld_tailq_entry) {
		if (ld->m_minor == minor(dev)) {
			/*! hacks */
			vm_mdl_t mdl;
			kassert(PGROUNDUP(buf) ==(vaddr_t) buf);
			kassert (buf >= (void*)HHDM_BASE && buf <= (void*)KHEAP_BASE);
			kassert (nbyte <= PGSIZE);
			mdl.nBytes = nbyte;
			mdl.nPages = 1;
			mdl.offset = 0;
			mdl.pages[0] = vm_page_from_paddr((paddr_t)V2P(buf));
			return [ld readBytes:nbyte at:off intoBuffer:&mdl completion:NULL];
		}
	}
	return -ENXIO;
}


static int
ld_write(dev_t dev, void *buf, size_t nbyte, off_t off)
{
	DKLogicalDisk *ld;
	TAILQ_FOREACH (ld, &ld_tailq, m_ld_tailq_entry) {
		if (ld->m_minor == minor(dev)) {
			/*! hacks */
			vm_mdl_t mdl;
			kassert(PGROUNDUP(buf) ==(vaddr_t) buf);
			kassert (buf >= (void*)HHDM_BASE && buf <= (void*)KHEAP_BASE);
			kassert (nbyte <= PGSIZE);
			mdl.nBytes = nbyte;
			mdl.nPages = 1;
			mdl.offset = 0;
			mdl.pages[0] = vm_page_from_paddr((paddr_t)V2P(buf));
			return [ld writeBytes:nbyte at:off fromBuffer:&mdl completion:NULL];
		}
	}
	return -ENXIO;
}


+ (void)initialize
{
	cdevsw_t cdev = { 0 };
	cdev.is_tty = false;
	cdev.private = self;
	cdev.open = ld_open;
	cdev.read = ld_read;
	cdev.write = ld_write;
	major = cdevsw_attach(&cdev);
}

- (blksize_t)blockSize
{
	return [m_underlying blockSize];
}

- (int)buildPosixDeviceName:(char *)buffer withMaxSize:(size_t)size
{
	int r;

	if ([m_underlying isKindOfClass:[DKDrive class]])
		return ksnprintf(buffer, size, "dk%d",
		    [(DKDrive *)m_underlying driveID]);

	r = [(id)m_underlying buildPosixDeviceName:buffer withMaxSize:size];
	buffer += r;
	size -= r;
	assert(size > 0);

	return ksnprintf(buffer, size, "s%lu", m_location);
}

- initWithUnderlyingDisk:(DKDevice<DKAbstractDiskMethods> *)underlying
		    base:(off_t)base
		    size:(size_t)size
		    name:(const char *)aname
		location:(size_t)location
		provider:(DKDevice *)provider
{
	char nameBuf[64];
	int  r;

	self = [super initWithProvider:provider];
	if (!self) {
		[self release];
		return NULL;
	}

	kmem_asprintf(&m_name, "%s Disk", aname);
	[self registerDevice];
	DKLogAttachExtra(self, "%lu MiB", size / 1024 / 1024);

	m_underlying = underlying;
	m_base = base;
	m_size = size;
	m_location = location;

	[self buildPosixDeviceName:nameBuf withMaxSize:63];

	DKDevLog(self, "POSIX DevFS node: %s\n", nameBuf);
	m_minor = minor++;
	TAILQ_INSERT_TAIL(&ld_tailq, self, m_ld_tailq_entry);
	r = devfs_make_node(makedev(major, m_minor), nameBuf);
	assert(r >= 0);

	if (![m_underlying isKindOfClass:[DKDrive class]]) {
		// int mountit(DKLogicalDisk * disk);
		// mountit(self);
	}

	if (location == 0) {
		[GPTVolumeManager probe:self];
	}

	return self;
}

- (int)readBytes:(size_t)nBytes
	      at:(off_t)offset
      intoBuffer:(vm_mdl_t *)buf
      completion:(struct dk_diskio_completion *)completion
{
	if (offset + nBytes > m_size)
		return -EINVAL;

	return [m_underlying readBytes:nBytes
				    at:offset + m_base
			    intoBuffer:buf
			    completion:completion];
}

- (int)writeBytes:(size_t)nBytes
	       at:(off_t)offset
       fromBuffer:(vm_mdl_t *)buf
       completion:(struct dk_diskio_completion *)completion
{
	if (offset + nBytes > m_size)
		return -EINVAL;

	return [m_underlying writeBytes:nBytes
				     at:offset + m_base
			     fromBuffer:buf
			     completion:completion];
}

@end
