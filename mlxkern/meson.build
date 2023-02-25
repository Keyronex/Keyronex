mlxkern_sources = files(
	'libkern/libkern.c',

	'object/object.c',

	'process/ps.c',
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
mlxkern_sources += uuid_sources

subdir('devmgr')
subdir('executive')
subdir('kernel')
subdir('vm')

if (arch == 'x86_64')
	subdir('machdep/amd64')
endif
