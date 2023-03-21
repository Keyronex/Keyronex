project('Keyronex', ['c', 'cpp'],
	default_options: ['cpp_std=gnu++20'])

arch = target_machine.cpu_family()
os = target_machine.system()

if not meson.is_cross_build()
	error('Keyronex must be cross-compiled')
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
		'-D_KERNEL'
	]
	freestanding_cpp_args = freestanding_c_args + '-fno-exceptions' + '-fno-rtti'
	freestanding_ld_args = ['-nostdlib', '-mno-red-zone']
else
	message('arch ' + arch + ' is not supported by Keyronex')
	error('bad arch')
endif

if (os != 'elf' and os != 'keyronex')
	message('compiler for os ' + os +
		' is not suitable for building Keyronex\n')
	message('the required compiler is either ' + arch + '-elf or ' +
		arch + '-keyronex')
	error('bad compiler')
endif

# limine dependencies
limine_include_directories = include_directories('vendor/limine-binary')
executable(
	'limine-deploy',
	'vendor/limine-binary/limine-deploy.c',
	native: true
)

# miscellaneous tools
bin2c = executable('bin2c', 'tools/bin2c.c', native: true)

mlibc = subproject('mlibc', default_options: [ 'prefix=/usr', 'libdir=lib' ])
#mlibc_keyronex_includes = mlibc.get_variable('keyronex_includes')
mlibc_keyronex_includes = mlibc.get_variable('libc_include_dirs')

uuid_sources = files('vendor/uuid/uuid.c')

subdir('vendor/lai')
subdir('vendor/lwip')

krx_sdk_includes = [ include_directories(
	'vendor/bsd-includes',
	'vendor/uuid',
	'kernel/_sdkinc'
	), mlibc_keyronex_includes, lwip_include_directories ]

subdir('driver')
subdir('kernel')
