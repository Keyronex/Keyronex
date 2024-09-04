
#include "dev/PS2Keyboard.h"
#include "kdk/amd64/portio.h"
#include "platform/amd64/IOAPIC.h"
#include "uacpi/resources.h"
#include "uacpi/utilities.h"

enum {
	kPS2OutputBufferFull = 0x1,
	kPS2InputBufferFull = 0x2,
};

/* executive/kconsole.c - temporary */
void ex_console_input(int c);

static void dpc_handler(void *arg);
static bool intr_handler(md_intr_frame_t *frame, void *arg);

static const char codes[128] = { '\0', '\e', '1', '2', '3', '4', '5', '6', '7',
	'8', '9', '0', '-', '=', '\b', '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u',
	'i', 'o', 'p', '[', ']', '\r', '\0', 'a', 's', 'd', 'f', 'g', 'h', 'j',
	'k', 'l', ';', '\'', '`', '\0', '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm',
	',', '.', '/', '\0', '\0', '\0', ' ', '\0' };

static const char codes_shifted[] = { '\0', '\e', '!', '@', '#', '$', '%', '^',
	'&', '*', '(', ')', '_', '+', '\b', '\t', 'Q', 'W', 'E', 'R', 'T', 'Y',
	'U', 'I', 'O', 'P', '{', '}', '\r', '\0', 'A', 'S', 'D', 'F', 'G', 'H',
	'J', 'K', 'L', ':', '"', '~', '\0', '|', 'Z', 'X', 'C', 'V', 'B', 'N',
	'M', '<', '>', '?', '\0', '\0', '\0', ' ' };

@implementation PS2Keyboard

uacpi_ns_iteration_decision
mouse_dev_cb(void *user, uacpi_namespace_node *node)
{
	struct ps2_info *info = user;

	kassert(info->mouse_node == NULL);
	info->mouse_node = node;
	return UACPI_NS_ITERATION_DECISION_BREAK;
}

static uacpi_resource_iteration_decision
resource_cb(void *user, uacpi_resource *resource)
{
	struct ps2_info *info = user;

	if (resource->type == UACPI_RESOURCE_TYPE_IRQ) {
		info->gsi = resource->irq.irqs[0];
	} else if (resource->type == UACPI_RESOURCE_TYPE_FIXED_IO) {
		if (info->cmd_port == (uint16_t)-1)
			info->cmd_port = resource->fixed_io.address;
		else
			info->data_port = resource->fixed_io.address;

	} else if (resource->type == UACPI_RESOURCE_TYPE_IO) {
		if (info->cmd_port == (uint16_t)-1)
			info->cmd_port = resource->io.minimum;
		else
			info->data_port = resource->io.minimum;
	}

	return UACPI_RESOURCE_ITERATION_CONTINUE;
}

static uacpi_resource_iteration_decision
mouse_resource_cb(void *user, uacpi_resource *resource)
{
	struct ps2_info *info = user;

	if (resource->type == UACPI_RESOURCE_TYPE_IRQ)
		info->mouse_gsi = resource->irq.irqs[0];

	return UACPI_RESOURCE_ITERATION_CONTINUE;
}

+ (BOOL)findResources:(uacpi_namespace_node *)node
		 info:(out struct ps2_info *)info
{
	uacpi_resources *resources;
	const uacpi_char *const mouse_ids[] = { "PNP0F03", "PNP0F13", "VMW0003",
		NULL };
	int r;

	uacpi_find_devices_at(uacpi_namespace_get_predefined(
				  UACPI_PREDEFINED_NAMESPACE_SB),
	    mouse_ids, mouse_dev_cb, info);

	r = uacpi_get_current_resources(node, &resources);
	kassert(r == UACPI_STATUS_OK);

	r = uacpi_for_each_resource(resources, resource_cb, info);
	kassert(r == UACPI_STATUS_OK);

	uacpi_free_resources(resources);

	if (info->mouse_node) {
		r = uacpi_get_current_resources(info->mouse_node, &resources);
		kassert(r == UACPI_STATUS_OK);

		r = uacpi_for_each_resource(resources, mouse_resource_cb,
		    info);
		kassert(r == UACPI_STATUS_OK);

		uacpi_free_resources(resources);
	}

	kprintf("PS/2: CMD 0x%x, DATA 0x%x, Keyboard GSI %d, Mouse GSI %d\n",
	    info->cmd_port, info->data_port, info->gsi, info->mouse_gsi);
	return (info->cmd_port != (uint16_t)-1 && info->gsi != (uint8_t)-1);
}

- (instancetype)initWithProvider:(DKACPIPlatform *)provider
			    info:(struct ps2_info *)info
{
	dk_interrupt_source_t source;
	int r;

	self = [super initWithProvider:provider];
	if (self == nil)
		return nil;

	m_state.dev = self;

	m_state.info = *info;
	m_state.head = m_state.tail = m_state.count = 0;

	m_state.dpc.arg = &m_state;
	m_state.dpc.callback = dpc_handler;
	m_state.dpc.cpu = NULL;

	source.id = isa_intr_overrides[info->gsi].gsi;
	source.edge = isa_intr_overrides[info->gsi].edge;
	source.low_polarity = isa_intr_overrides[info->gsi].lopol;

	r = [IOApic handleSource:&source
		     withHandler:intr_handler
			argument:&m_state
		      atPriority:kIPLDevice
			   entry:&m_intrEntry];
	if (r != 0) {
		DKDevLog(self, "failed to register interrupt handler: %d", r);
		[self release];
		return nil;
	}

	return self;
}

+ (BOOL)probeWithProvider:(DKACPIPlatform *)provider
		 acpiNode:(uacpi_namespace_node *)node
{
	struct ps2_info info = { NULL, -1, -1, -1, -1 };

	[self findResources:node info:&info];
	if (info.cmd_port == (uint16_t)-1 || info.gsi == (uint8_t)-1)
		return NO;

	[[self alloc] initWithProvider:provider info:&info];

	return NO;
}

static void
translateScancode(struct ps2_kb_state *state, uint8_t code)
{
	switch (code) {
	case 0x2a:
	case 0x36:
	case 0xaa:
	case 0xb6:
		state->isShifted = code & 0x80 ? false : true;
		return;

	case 0x1d:
	case 0x9d:
		state->isCtrled = code & 0x80 ? false : true;
		return;

	case 0x4b: /* left */
		return (void)kprintf("\e[D");

	case 0x4d: /* right */
		return (void)kprintf("\e[C");

	case 0x48: /* up */
		return (void)kprintf("\e[A");

	case 0x50: /* down */
		return (void)kprintf("\e[B");

	default:
	    /* epsilon */
	    ;
	}

	if (!(code & 0x80)) {
		char ch;

		if (state->isShifted)
			ch = codes_shifted[code];
		else
			ch = codes[code];

		if (state->isCtrled) {
			if (ch >= 'a' && ch <= 'z')
				ch -= 32;
			ch -= 64;
		}

		ex_console_input(ch);
	}
}

static void
dpc_handler(void *arg)
{
	struct ps2_kb_state *state = arg;

	while (true) {
		ipl_t ipl = splraise(kIPLDevice);
		uint8_t val;

		if (state->count == 0) {
			splx(ipl);
			break;
		}

		val = state->scancodeBuf[state->tail++];
		state->tail %= sizeof(state->scancodeBuf);
		state->count--;
		splx(ipl);

		translateScancode(state, val);
	}
}

static bool
intr_handler(md_intr_frame_t *frame, void *arg)
{
	struct ps2_kb_state *state = arg;
	uint8_t sc;

	if (!(inb(state->info.data_port) & kPS2OutputBufferFull))
		return false;

	sc = inb(0x60);
	if (state->count == sizeof(state->scancodeBuf))
		kfatal("out of space in ring buffer");

	state->scancodeBuf[state->head++] = sc;
	state->head %= sizeof(state->scancodeBuf);
	state->count++;

	ke_dpc_enqueue(&state->dpc);

	return true;
}

@end
