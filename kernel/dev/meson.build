kernel_sources += files('setup_ddk.m', 'buf.c', 'DKDevice.m', 'Ext2FS.m',
	'DKObject.m', 'DKVirtIOMMIODevice.m', 'FBTerminal.m', 'VirtIODisk.m',
	'VirtIOGPU.m', 'iop.m', 'PCIBus.m', 'SimpleFB.m', 'vfs.c', 'DOSFS.m')
kernel_sources += files('../../vendor/limine-terminal/term.c',
	'../../vendor/limine-terminal/backends/framebuffer.c')
kernel_sources += [ bin2c_gen.process('key272x70.data'),
	bin2c_gen.process('netascale102x82.data')
]
if (cpu != 'm68k')
	kernel_sources += [ files('acpi/DKACPIPlatform.m', 'acpi/uacpi_integration.c'), uacpi_sources ]
	freestanding_include_dirs += uacpi_includes
endif
subdir(cpu)
