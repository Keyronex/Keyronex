kernel_sources += files('pci/ecam.c', 'DKAxis.m', 'DKDevice.m', 'ddk.m', 'iop.c')

if (cpu != 'm68k')
	kernel_sources += files('acpi/DKACPINode.m', 'acpi/DKACPIPlatform.m',
		'acpi/uacpi.m', 'pci/DKPCIBridge.m', 'pci/DKPCIDevice.m',)
	kernel_sources += uacpi_sources
	freestanding_c_args += [ '-DUACPI_FORMATTED_LOGGING=1' ]
	freestanding_include_dirs += uacpi_includes
endif
