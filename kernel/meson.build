project('Keyronex')

headers_only = get_option('headers_only')
no_headers = get_option('no_headers')
arch = get_option('arch')

if not no_headers
	subdir('include')
endif

if not headers_only
	add_languages('c', 'cpp', 'objc')

	freestanding_c_args = [
		'-fno-strict-aliasing',
		'-fwrapv',
		'-fno-delete-null-pointer-checks',
		'-fno-omit-frame-pointer',
	]

	if (arch == 'amd64')
		meson_cpu_family = 'x86_64'
		cpu = 'amd64'
		freestanding_c_args += [
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
			'-DAMD64',
		]
	elif (arch == 'm68k')
		cpu = 'm68k'
		meson_cpu_family = cpu
		freestanding_c_args += [
			'-DM68K=1',
			'-ffreestanding',
			'-fno-stack-protector',
			'-fno-stack-check',
			'-fno-pie',
			'-fno-pic',
		]
	elif (arch == 'aarch64')
		cpu = 'aarch64'
		meson_cpu_family = cpu
		freestanding_c_args += [
			'-DAARCH64',
			'-ffreestanding',
			'-fno-stack-protector',
			'-fno-stack-check',
			'-fno-pie',
			'-fno-pic',
			'-mgeneral-regs-only',
		]
	elif (arch == 'riscv64')
		cpu = 'riscv64'
		meson_cpu_family = cpu
		freestanding_c_args += [
			'-DRISCV64',
			'-ffreestanding',
			'-fno-stack-protector',
			'-fno-stack-check',
			'-fno-pie',
			'-fno-pic',
			'-mabi=lp64',
			'-march=rv64imac_zicsr_zifencei',
			'-mno-relax'
		]
	else
		message('\n\tport ' + arch + ' is not supported by Keyronex')
		error('\n\tbad arch')
	endif

	freestanding_c_args += [ '-D_KERNEL' ]
	freestanding_include_dirs = [ include_directories(
		'../vendor/include',
		'../vendor/include-private',
		'../vendor/limine',
		'include',
		'.'
	)]

	executable(
		'limine',
		'../vendor/limine/limine.c',
		native: true
	)

	bin2c = executable('bin2c', '../tools/bin2c.c', native: true)

	bin2c_gen = generator(bin2c,
		output : '@PLAINNAME@.c',
		arguments : [ '-i', '@INPUT@', '-o', '@OUTPUT@', '-s', '@BASENAME@'  ]
	)

	uacpi = subproject('uacpi')
	uacpi_sources = uacpi.get_variable('sources')
	uacpi_includes = uacpi.get_variable('includes')

	subdir('lwip')
	subdir('objfwrt')

	kernel_sources = [ files('libkern/libkern.c', 'libkern/murmurhash.c',
		'libkern/OSArray.m', 'libkern/OSDictionary.m', 'libkern/OSMapTable.m',
		'libkern/OSNumber.m', 'libkern/OSObject.m', 'libkern/OSString.m') ]

	subdir('dev')
	subdir('executive')
	subdir('fs')
	subdir('kdb')
	subdir('vfs')
	subdir('vm')
	subdir('net')
	subdir('kern')
	subdir('ntcompat')

	if (arch == 'amd64')
		subdir('platform/amd64')
	elif (arch == 'm68k')
		subdir('platform/m68k-virt')
	elif (arch == 'aarch64')
		subdir('platform/aarch64-virt')
	elif (arch == 'riscv64')
		subdir('platform/riscv64-virt')
	endif

endif
