project('Melantix', ['c', 'cpp'], default_options: ['cpp_std=gnu++20'])

arch = target_machine.cpu_family()
os = target_machine.system()

if not meson.is_cross_build()
	error('Melantix must be cross-compiled')
endif

if (arch == 'x86_64')
	freestanding_c_args = [
		'-ffreestanding',
		'-fno-stack-protector',
		'-fno-stack-check',
		'-fno-pie',
		'-fno-pic',
		'-m64',
		'-march=x86-64',
		'-mabi=sysv',
		'-mno-80387',
		'-mno-mmx',
		'-mno-sse',
		'-mno-sse2',
		'-mno-red-zone',
		'-mcmodel=kernel',
	]
	freestanding_cpp_args = freestanding_c_args
	freestanding_ld_args = ['-nostdlib', '-mno-red-zone']
else
	message('arch ' + arch + ' is not supported by Melantix')
	error('bad arch')
endif

if (os != 'elf' and os != 'melantix')
	message('compiler for os ' + os +
		' is not suitable for building Melantix\n')
	message('the required compiler is either ' + arch + '-elf or ' +
		arch + '-melantix')
	fatal('bad compiler')
endif

# limine dependencies
limine_include_directories = include_directories('external/limine-binary')
executable(
	'limine-deploy',
	'external/limine-binary/limine-deploy.c',
	native: true
)

mlx_sdk_includes = include_directories(
	'external/bsd-includes',
	'mlxsdk/include',
	'mlxkern/sdkinc'
)

# miscellaneous tools
bin2c = executable('bin2c', 'tools/bin2c.c', native: true)

subdir('virtiofam')
subdir('mlxkern')
