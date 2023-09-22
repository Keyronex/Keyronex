kernel_sources += files('setup_ddk.m', 'buf.c', 'DKDevice.m', 'Ext2FS.m',
	'DKObject.m', 'DKVirtIOMMIODevice.m', 'FBTerminal.m', 'VirtIODisk.m',
	'VirtIOGPU.m', 'iop.m', 'PCIBus.m', 'SimpleFB.m', 'vfs.c')
kernel_sources += files('../../vendor/limine-terminal/term.c',
	'../../vendor/limine-terminal/backends/framebuffer.c')
kernel_sources += [ bin2c_gen.process('key272x70.data'),
	bin2c_gen.process('netascale102x82.data')
]
if (cpu != 'm68k')
	kernel_sources += [ files('DKACPIPlatform.m'), lai_sources ]
	freestanding_include_dirs += lai_includes
endif
subdir(cpu)
