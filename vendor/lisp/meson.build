project('lisp', 'cpp', default_options: ['cpp_std=c++17'])


# Define targets
executable(
    'lisp',
	'loader/Entry.S',
    'loader/Loader.cpp',
    'loader/Main.cpp',
    'loader/Memman.cpp',
    'loader/Syslib.cpp',
    'loader/Util.cpp',

	cpp_args: [ '-ffreestanding', '-fno-rtti', '-fno-exceptions',
	'-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-m68040',
	'-DNPL_ENABLE_LOGGING'],
	link_args: [ '-nostdlib', '-static',
		'-Wl,-T' + meson.current_source_dir() + '/loader/Linker.lds'
	],
	include_directories: ['lib', 'thirdparty'],

#	dependencies: files(meson.current_build_dir() / '../kernel/platform/m68k-virt/keyronex')
)
