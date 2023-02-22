mlxkern_link_args += ['-Wl,-T' + meson.current_source_dir() + '/linker.ld']
mlxkern_sources += files('apic.c', 'cpu.c', 'intr.c', 'locore.S', 'start.c')

mlxkern = executable(
	'mlxkern',
	mlxkern_sources,
	c_args: [mlxkern_c_args],
	cpp_args: [mlxkern_cpp_args],
	link_args: [
		'-nostdlib',
		'-static',
		'-Wl,-z,max-page-size=0x1000',
		'-Wl,--build-id=none',
		mlxkern_link_args,
	],
	link_with: [
		drivers
	],
	include_directories: [ mlxkern_include_directories,
		limine_include_directories,
		'.' 
	]
)
