kernel_sources += files('DKAxis.m', 'DKDevice.m', 'FBTerminal.m', 'SimpleFB.m',
    'pci/ecam.c', 'ddk.m', 'iop.c', 'virtio/DKVirtIOTransport.m',)

kernel_sources += [
	bin2c_gen.process('key272x70.data'),
	bin2c_gen.process('netascale102x82.data'),
]

kernel_sources += files(
	'../../vendor/limine-terminal/backends/framebuffer.c',
	'../../vendor/limine-terminal/term.c',
)

if (cpu != 'm68k')
	kernel_sources += files('acpi/DKACPINode.m', 'acpi/DKACPIPlatform.m',
		'acpi/uacpi.m', 'pci/DKPCIBridge.m', 'pci/DKPCIDevice.m',
		'usb/DKUSBDevice.m', 'usb/DKUSBHub.m', 'usb/DKUSBKeyboard.m',
		'usb/xhci/XHCIController.m', 'virtio/VirtIOPCITransport.m',)
	kernel_sources += uacpi_sources
	freestanding_c_args += [
		'-DUACPI_FORMATTED_LOGGING=1',
		'-DUACPI_SIZED_FREES=1'
	]
	freestanding_include_dirs += uacpi_includes
endif
