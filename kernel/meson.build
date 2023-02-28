kernel_sources = files(
	'libkern/libkern.c',

	'object/object.c',

	'process/ps.c',
	)
kernel_c_args = [freestanding_c_args, '-D_KERNEL']
kernel_cpp_args = [freestanding_cpp_args]
kernel_link_args = []
kernel_include_directories = [ krx_sdk_includes, include_directories('./') ]

gen = generator(bin2c,
    output : '@PLAINNAME@.c',
    arguments : [ '-i', '@INPUT@', '-o', '@OUTPUT@', '-s', '@BASENAME@'  ]
)

kernel_sources += gen.process(['logosmall.data'])
kernel_sources += uuid_sources

subdir('devmgr')
subdir('executive')
subdir('kernel')
subdir('vm')

if (arch == 'x86_64')
	subdir('machdep/amd64')
endif
