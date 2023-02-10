#include <kern/kmem.h>
#include <libkern/libkern.h>
#include <vm/vm.h>

#include <string.h>
#include <vfs/vfs.h>

struct swapdev {
	size_t	 nused;
	size_t	 npages;
	vnode_t *vn;
	uint8_t	 bitmap[0];
};

#define BITMAP_SIZE(NENTRIES) ROUNDUP(NENTRIES, 8) / 8

#define BIT_SET(bitmap, bit) bitmap |= (1 << bit)
#define BIT_CLEAR(bitmap, bit) bitmap &= ~(1 << bit)
#define BIT_VAL(bitmap, bit) ((0u == (bitmap & (1 << bit))) ? 0u : 1u)

#define SWAPDEV_SIZE(NPAGES) (sizeof(struct swapdev) + BITMAP_SIZE(npages))

static inline unsigned
bmap_u8_set(uint8_t *map, unsigned value)
{
	size_t base = value / 8;
	size_t off = value % 8;
	return BIT_SET(map[base], off);
}

static inline unsigned
bmap_u8_clr(uint8_t *map, unsigned value)
{
	size_t base = value / 8;
	size_t off = value % 8;
	return BIT_CLEAR(map[base], off);
}

static inline unsigned
bmap_u8_val(uint8_t *map, unsigned value)
{
	size_t base = value / 8;
	size_t off = value % 8;
	return BIT_VAL(map[base], off);
}

struct swapdev *swapdev;

struct swapdev *
swapdev_new(vnode_t *vn, size_t nbytes)
{
	size_t npages = nbytes / PGSIZE;
	swapdev = kmem_alloc(SWAPDEV_SIZE(npages));
	swapdev->vn = vn;
	swapdev->npages = npages;
	memset(swapdev->bitmap, 0x0, BITMAP_SIZE(npages));
	return 0;
}

static drumslot_t
swapdev_allocslot(struct swapdev *sd)
{
	for (unsigned i = 0; i < sd->npages; i++)
		if (bmap_u8_val(sd->bitmap, i) == 0) {
			bmap_u8_set(sd->bitmap, i);
			sd->nused++;
			return i;
		}

	return -1;
}

int
vm_swapon(const char *name)
{
	vnode_t *vn, *devvn = NULL;
	int	 r;

	kprintf("vm_swapon(%s)\n", name);

	r = vfs_lookup(dev_vnode, &vn, name, 0, NULL);
	if (r != 0) {
		kprintf("vm_swapon: failed to lookup %s: %d\n", name, r);
		return r;
	}

	r = VOP_OPEN(vn, &devvn, 0);
	if (r != 0) {
		kprintf("vm_swapon: failed to open %s: %d\n", name, r);
		return r;
	}

	if (devvn == NULL) {
		devvn = vn;
		vn = NULL;
	}

	swapdev_new(devvn, 32 * 1024 * 1024);

	return 0;
}

vm_pager_ret_t
vm_swp_pagein(vm_page_t *page, drumslot_t slot)
{
	int r;

	kassert(page->is_anon);
	kassert(page->busy);

#if DEBUG_SWAP == 1
	nk_dbg("vm_swap_pagein(page 0x%lx, slot %lu)\n", page->paddr, slot);
#endif
	r = VOP_READ(swapdev->vn, P2V(page->paddr), PGSIZE, PGSIZE * slot);
	kassert(r == PGSIZE);

	page->busy = false;

	vm_page_changequeue(page, &vm_pgwiredq, &vm_pgactiveq);

	return kVMPagerOK;
}

vm_pager_ret_t
vm_swp_pageout(vm_page_t *page)
{
	drumslot_t slot;
	int	   r;

	kassert(page->is_anon);
	kassert(page->busy);

	slot = swapdev_allocslot(swapdev);
	if (slot == -1) {
		nk_fatal("drumslots exhausted\n");
		return kVMPagerExhausted;
	}

#if DEBUG_SWAP == 1
	nk_dbg("got drumslot %lu, doing write....\n", slot);
#endif
	r = VOP_WRITE(swapdev->vn, P2V(page->paddr), PGSIZE, PGSIZE * slot);
	kassert(r == PGSIZE);

	page->anon->resident = false;
	page->anon->drumslot = slot;

	page->busy = false;
	page->is_anon = false;
	page->accessed = false;
	page->dirty = false;

	vm_page_changequeue(page, &vm_pginactiveq, &vm_pgfreeq);

	return kVMPagerOK;
}

void
vm_swapstat(void)
{

	kprintf("       Swapdev Stats\n")
	    kprintf("\033[7m%-9s%-9s%-9s\033[m\n", "swapdev", "free", "used");
	kprintf("%-9zu%-9zu%-9zu\n", 0lu, swapdev->npages - swapdev->nused,
	    swapdev->nused);
}
