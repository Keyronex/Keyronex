keyronex = executable('keyronex',
	'entry.c', 'intr.c', 'locore.S', 'VirtAArch64Platform.m', '../generic/entry.c', kernel_sources,
	c_args: [ freestanding_c_args, '-std=gnu2x' ],
	objc_args: [ freestanding_c_args, '-std=gnu17'] ,
	include_directories: [ freestanding_include_dirs ],
	link_args: [ '-nostdlib', '-static', '-lgcc', '-Wl,-z', 'max-page-size=0x1000',
		'-Wl,-T' + meson.current_source_dir() + '/linker.ld'
	],
	dependencies: [ kobjcrt_dep ]
)
