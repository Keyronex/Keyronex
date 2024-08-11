keyronex = executable('keyronex',
	'APLIC.m', 'DKACPIPlatform+RISCV.m', 'entry.c', 'intr.c','VirtRISCV64Platform.m',
	'../generic/entry.c', kernel_sources,
	c_args: [ freestanding_c_args, '-std=gnu2x' ],
	objc_args: [ freestanding_c_args, '-std=gnu17'] ,
	include_directories: [ freestanding_include_dirs ],
	link_args: [ '-nostdlib', '-static', '-Wl,-z', 'max-page-size=0x1000',
		meson.source_root() / '../vendor/libgcc_rv64.a',
		'-Wl,-T' + meson.current_source_dir() + '/linker.ld'
	],
	dependencies: [ kobjcrt_dep ]
)
