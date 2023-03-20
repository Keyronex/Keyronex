gen = generator(bin2c,
    output : '@PLAINNAME@.c',
    arguments : [ '-i', '@INPUT@', '-o', '@OUTPUT@', '-s', '@BASENAME@'  ]
)

kernel_c_args = [freestanding_c_args, '-D_KERNEL']
kernel_cpp_args = [freestanding_cpp_args]
kernel_link_args = []
kernel_include_directories = [ krx_sdk_includes, lwip_include_directories,
	include_directories('./') ]

kernel_sources = []
kernel_sources += gen.process(['logosmall.data'])
kernel_sources += uuid_sources
kernel_sources += lwip_sources

kernel_sources += files(
	'libkern/libkern.c',

	'object/object.c',

	'process/ps.c',
)

subdir('devmgr')
subdir('executive')
subdir('kernel')
subdir('keysock')
subdir('posix')
subdir('vm')

if (arch == 'x86_64')
	subdir('machdep/amd64')
endif
