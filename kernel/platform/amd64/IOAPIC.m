#if 0

#include "IOAPIC.h"
#include "dev/acpi/DKAACPIPlatform.h"
#include "kdk/executive.h"
#include "kdk/kmem.h"
#include "kdk/object.h"

enum {
	kDeliveryModeFixed = 0x0,
	kDeliveryModeLowPriority = 0x1,
	kDeliveryModeSmi = 0x2,
	kDeliveryModeNmi = 0x4,
	kDeliveryModeInit = 0x5,
	kDeliveryModeExtInt = 0x7
};

enum {
	kDestinationModePhysical = 0x0,
	kDestinationModeLogical = 0x1,
};

enum {
	kRegisterId = 0x0,
	kRegisterVersion = 0x1,
	kRegisterArbitrationPriority = 0x2,
	kRegisterRedirectionEntriesBase = 0x10,
};

struct isa_intr_override isa_intr_overrides[16];

static inline uint32_t
redirection_register(uint32_t entry)
{
	return kRegisterRedirectionEntriesBase + entry * 2;
}

static uint32_t
ioapic_read(vaddr_t vaddr, uint32_t reg)
{
	volatile uint32_t *base = (volatile uint32_t *)vaddr;
	base[0] = reg;
	return base[4];
}

static void
ioapic_write(vaddr_t vaddr, uint32_t reg, uint32_t val)
{
	volatile uint32_t *base = (volatile uint32_t *)vaddr;
	base[0] = reg;
	base[4] = val;
}

static void
ioapic_route(vaddr_t vaddr, uint8_t i, uint8_t vec, bool lopol, bool edge)
{
	uint64_t ent = vec;
	ent |= kDeliveryModeFixed << 8;
	ent |= kDestinationModePhysical << 11;
	ent |= 0ul << 56; /* lapic id 0 */
	if (lopol)
		ent |= 1 << 13; /* polarity low */
	if (!edge)
		ent |= 1 << 15; /* level triggered */
	ioapic_write(vaddr, redirection_register(i), ent);
	ioapic_write(vaddr, redirection_register(i) + 1, ent >> 32);
}

static TAILQ_TYPE_HEAD(, IOApic) ioapics = TAILQ_HEAD_INITIALIZER(ioapics);

@implementation IOApic

- initWithProvider:(DKDevice *)provider
		id:(uint32_t)id
	   address:(paddr_t)paddr
	   gsiBase:(uint32_t)gsiBase
{
	int r;

	self = [super initWithProvider:provider];

	r = vm_ps_map_physical_view(kernel_process->vm, &_vaddr, PGSIZE, paddr,
	    kVMAll, kVMAll, false);
	kassert(r == 0);

	_id = id;
	_gsi_base = gsiBase;
	_n_redirs = ((ioapic_read(_vaddr, kRegisterVersion) >> 16) & 0xff) + 1;
	kassert(_n_redirs <= 240);

	redirs = kmem_alloc(sizeof(uint8_t) * _n_redirs);
	for (int i = 0; i < _n_redirs; i++)
		redirs[i] = 0;

	TAILQ_INSERT_TAIL(&ioapics, self, _ioapics_entries);

	kmem_asprintf(obj_name_ptr(self), "ioapic-%d", id);
	[self registerDevice];
	DKLogAttach(self);

	return self;
}

+ (int)handleSource:(dk_interrupt_source_t *)source
	withHandler:(intr_handler_t)handler
	   argument:(void *)arg
	 atPriority:(ipl_t)prio
	      entry:(struct intr_entry *)entry
{
	IOApic *ioapic;
	uint8_t gsi = source->id;
	bool found = false;

	TAILQ_FOREACH (ioapic, &ioapics, _ioapics_entries) {
		if (ioapic->_gsi_base <= gsi &&
		    ioapic->_gsi_base + ioapic->_n_redirs > gsi) {
			uint8_t vec;
			uint8_t intr = gsi - ioapic->_gsi_base;
			int r;

			kassert(ioapic->redirs[intr] == 0 && "shared");

			r = md_intr_alloc("gsi", prio, handler, arg,
			    !source->edge, &vec, entry);
			if (r != 0) {
				kprintf(
				    "ioapic: failed to register interrupt for GSI %d: "
				    "md_intr_alloc returned %d\n",
				    gsi, r);
				return -1;
			}

			ioapic_route(ioapic->_vaddr, intr, vec, source->low_polarity,
			    source->edge);
			ioapic->redirs[intr] = vec;

			found = true;
			break;
		}
	}

	if (!found) {
		DKLog("IOApic", "no I/O APIC found for GSI %d\n", gsi);
		return -1;
	}

	return 0;
}

@end
#endif
