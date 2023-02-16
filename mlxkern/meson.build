mlxkern_sources = files(
	'ke/ke.c', 'ke/msgqueue.c', 'ke/sched.c', 'ke/semaphore.c', 'ke/thread.c',
	'ke/time.c', 'ke/timer.c', 'ke/wait.c',

	'libkern/libkern.c',

	'object/object.c',

	'ps/ps.c',

	'vm/kmem_slab.c', 'vm/kwired.c', 'vm/fork.c', 'vm/page.c', 'vm/vmem.c', 'vm/ws.c', 
	)
mlxkern_c_args = [freestanding_c_args, '-D_KERNEL']
mlxkern_cpp_args = [freestanding_cpp_args]
mlxkern_link_args = []
mlxkern_include_directories = [ mlx_sdk_includes, include_directories('./') ]

gen = generator(bin2c,
    output : '@PLAINNAME@.c',
    arguments : [ '-i', '@INPUT@', '-o', '@OUTPUT@', '-s', '@BASENAME@'  ]
)

mlxkern_sources += gen.process(['logosmall.data'])

if (arch == 'x86_64')
	mlxkern_sources += files('vm/amd64/vmamd64.c')
	subdir('hl/amd64')
endif
