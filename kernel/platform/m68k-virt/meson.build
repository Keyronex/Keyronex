keyronex = executable('keyronex',
	'../generic/entry.c', 'intr.c', 'locore.S', 'goldfish_pic.c', 'platform.c',
	'goldfish_rtc.c', 'goldfish_tty.c', 'Virt68kPlatform.m', kernel_sources,
	c_args: [ freestanding_c_args, '-mcpu=68040', '-std=gnu2x' ],
	objc_args: [ freestanding_c_args, '-mcpu=68040', '-std=gnu17'] ,
	include_directories: freestanding_include_dirs,
	link_args: [ '-nostdlib', '-static', '-lgcc',
		'-Wl,-T' + meson.current_source_dir() + '/linker.ld'
	],
	dependencies: [ kobjcrt_dep ]
)
