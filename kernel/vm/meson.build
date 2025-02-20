kernel_sources += files('copyinout.c', 'fault.c', 'fork.c', 'init.c',
	'kmem_cache.c', 'kwired.c', 'resident.c', 'tables.c', 'ubc.c', 'map.c',
	'vmbalance.c', 'vmem.c', 'writeback.c', 'ws.c')

if (cpu == 'm68k')
	kernel_sources += files('m68k/vm_m68k.c')
elif (cpu == 'aarch64')
	kernel_sources += files('aarch64/vm_aarch64.c')
elif (cpu == 'amd64')
	kernel_sources += files('amd64/vm_amd64.c')
elif (cpu == 'riscv64')
	kernel_sources += files('riscv64/vm_riscv64.c')
else
	#error('Unknown processor')
endif
