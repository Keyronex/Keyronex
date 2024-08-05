kernel_sources = [ files('libkern/libkern.c') ]

subdir('dev')
subdir('executive')
subdir('fs')
subdir('kdb')
subdir('vfs')
subdir('vm')
subdir('net')
subdir('kern')
subdir('ntcompat')

if (arch == 'amd64')
	subdir('platform/amd64')
elif (arch == 'm68k')
	subdir('platform/m68k-virt')
elif (arch == 'aarch64')
	subdir('platform/aarch64-virt')
elif (arch == 'riscv64')
	subdir('platform/riscv64-virt')
endif
