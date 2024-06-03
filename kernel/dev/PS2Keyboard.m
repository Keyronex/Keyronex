
#include "dev/PS2Keyboard.h"
#include "uacpi/resources.h"
#include "uacpi/utilities.h"

@implementation PS2Keyboard

struct ps2_info {
	uacpi_namespace_node *mouse_node;
	uint16_t cmd_port, data_port;
	uint8_t gsi, mouse_gsi;
};

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
{
	struct ps2_info info = { NULL, -1, -1, -1, -1 };
	uacpi_resources *resources;
	const uacpi_char *const mouse_ids[] = { "PNP0F03", "PNP0F13", "VMW0003",
		NULL };
	int r;

	uacpi_find_devices_at(uacpi_namespace_get_predefined(
				  UACPI_PREDEFINED_NAMESPACE_SB),
	    mouse_ids, mouse_dev_cb, &info);

	r = uacpi_get_current_resources(node, &resources);
	kassert(r == UACPI_STATUS_OK);

	r = uacpi_for_each_resource(resources, resource_cb, &info);
	kassert(r == UACPI_STATUS_OK);

	uacpi_free_resources(resources);

	if (info.mouse_node) {
		r = uacpi_get_current_resources(info.mouse_node, &resources);
		kassert(r == UACPI_STATUS_OK);

		r = uacpi_for_each_resource(resources, mouse_resource_cb,
		    &info);
		kassert(r == UACPI_STATUS_OK);

		uacpi_free_resources(resources);
	}

	kprintf("PS/2: CMD 0x%x, DATA 0x%x, Keyboard GSI %d, Mouse GSI %d\n",
	    info.cmd_port, info.data_port, info.gsi, info.mouse_gsi);
	return (info.cmd_port != (uint16_t)-1 && info.gsi != (uint8_t)-1);
}

+ (BOOL)probeWithProvider:(DKACPIPlatform *)provider
		 acpiNode:(uacpi_namespace_node *)node
{
	[self findResources:node];

	return NO;
}

@end
