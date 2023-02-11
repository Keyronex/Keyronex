mlxkern_sources = []
mlxkern_c_args = [freestanding_c_args]
mlxkern_cpp_args = [freestanding_cpp_args]
mlxkern_link_args = []
mlxkern_include_directories = []

gen = generator(bin2c,
    output : '@PLAINNAME@.c',
    arguments : [ '-i', '@INPUT@', '-o', '@OUTPUT@', '-s', '@BASENAME@'  ]
)

mlxkern_sources += gen.process(['logosmall.data'])

if (arch == 'x86_64')
	subdir('hl/amd64')
endif
