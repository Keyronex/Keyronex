/*
 * Copyright (c) 2024-2025 NetaScale Object Solutions.
 * Created on Wed Apr 17 2024.
 */

#include <ddk/DKAxis.h>
#include <kdk/kern.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

#include "dev/acpi/DKACPIPlatform.h"

void DKLogAttach(DKDevice *child, DKDevice *parent);

DKAxis *gACPIAxis;

@implementation DKACPIPlatform

- (instancetype)init
{
	int r;

	gACPIAxis = [DKAxis axisWithName:"DKACPI"];

	r = uacpi_initialize(0);
	kassert(r == UACPI_STATUS_OK);

	r = uacpi_namespace_load();
	kassert(r == UACPI_STATUS_OK);

	r = uacpi_namespace_initialize();
	kassert(r == UACPI_STATUS_OK);

	r = uacpi_set_interrupt_model(UACPI_INTERRUPT_MODEL_IOAPIC);
	kassert(r == UACPI_STATUS_OK);

	self = [super initWithNamespaceNode:uacpi_namespace_root()];
	[gACPIAxis addChild:self ofParent:nil];
	DKLogAttach(self, nil);

	return self;
}

- (void)start
{
	[super start];
	[DKDevice drainStartQueue];
}

@end
