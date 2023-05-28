project('Keyronex', default_options: ['cpp_std=gnu++20'])

headers_only = get_option('headers_only')
no_headers = get_option('no_headers')

if not headers_only
	arch = target_machine.cpu_family()
	os = target_machine.system()

	add_languages('c', 'cpp', 'objc')

	if not meson.is_cross_build()
		error('Keyronex must be cross-compiled')
	endif

	if (arch == 'x86_64')
		freestanding_c_args = [
			'-ffreestanding',
			'-fno-stack-protector',
			'-fno-stack-check',
			'-fno-strict-aliasing',
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
		freestanding_objc_args = freestanding_c_args + [
			'-fconstant-string-class=OFConstantString', '-std=gnu17',
			'-fobjc-exceptions'
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
	bin2c = executable('bin2c', '../tools/bin2c.c', native: true)

	bin2c_gen = generator(bin2c,
		output : '@PLAINNAME@.c',
		arguments : [ '-i', '@INPUT@', '-o', '@OUTPUT@', '-s', '@BASENAME@'  ]
	)

	#mlibc = subproject('mlibc', default_options: [ 'prefix=/usr', 'libdir=lib' ])
	#mlibc_keyronex_includes = mlibc.get_variable('keyronex_includes')
	#mlibc_keyronex_includes = mlibc.get_variable('libc_include_dirs')

	uuid_sources = files('vendor/uuid/uuid.c')

	subdir('vendor/lai')
	subdir('vendor/lwip')

	krx_sdk_includes = [ include_directories(
		'vendor/bsd-includes',
		'vendor/uuid',
		'_sdkinc',
		'../libdxf'
		), limine_include_directories,
		lwip_include_directories ]

	subdir('driver')

	kernel_c_args = [freestanding_c_args, '-D_KERNEL']
	kernel_objc_args = [freestanding_objc_args, '-D_KERNEL']
	kernel_cpp_args = [freestanding_cpp_args]
	kernel_link_args = []
	kernel_include_directories = [ krx_sdk_includes,
		include_directories('./') ]

	kernel_sources = []
	kernel_sources += bin2c_gen.process(['logosmall.data'])
	kernel_sources += uuid_sources
	kernel_sources += lwip_sources

	kernel_sources += files(
		'vendor/libkern/openbsd.c',

		'libkern/libkern.c',

		'object/object.c',

		'process/ps.c',
	)

	subdir('devmgr')
	subdir('executive')
	subdir('kernel')
	subdir('keysock')
	subdir('posix')
	subdir('vm')
	subdir('kobjc')

	if (arch == 'x86_64')
		subdir('machdep/amd64')
	endif
endif

if not no_headers
	subdir('_sdkinc')
endif
