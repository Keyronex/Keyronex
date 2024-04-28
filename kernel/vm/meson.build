kernel_sources += files('vmbalance.c', 'writeback.c', 'fault.c', 'kmem_slab.c',
	'kwired.c', 'resident.c', 'tables.c', 'map.c', 'vmem.c', 'ws.c')

if (cpu == 'm68k')
	kernel_sources += files('m68k/vm_m68k.c')
elif (cpu == 'aarch64')
	kernel_sources += files('aarch64/vm_aarch64.c')
elif (cpu == 'amd64')
	kernel_sources += files('amd64/vm_amd64.c')
else
	#error('Unknown processor')
endif
