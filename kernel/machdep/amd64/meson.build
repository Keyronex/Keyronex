kernel_link_args += ['-Wl,-T' + meson.current_source_dir() + '/linker.ld']
kernel_sources += files('apic.c', 'cpu.c', 'intr.c', 'locore.S', 'start.c')

kernel = executable(
	'kernel',
	kernel_sources,
	c_args: [kernel_c_args],
	cpp_args: [kernel_cpp_args],
	link_args: [
		'-nostdlib',
		'-static',
		'-Wl,-z,max-page-size=0x1000',
		'-Wl,--build-id=none',
		kernel_link_args,
	],
	link_with: [
		drivers
	],
	include_directories: [ kernel_include_directories,
		limine_include_directories,
		'.' 
	]
)
