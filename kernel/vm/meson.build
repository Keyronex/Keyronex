kernel_sources += files('fault.c', 'fork.c', 'kmem_slab.c', 'kwired.c',
	'page.c', 'paging.c', 'vad.c', 'vmem.c', 'ws.c')

if (arch == 'x86_64')
	kernel_sources += files('amd64/useramd64.c', 'amd64/vmamd64.c')
endif
