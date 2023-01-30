project('Keyronex', ['c', 'cpp', 'objc'], default_options: [ 'cpp_std=gnu++20' ])

arch = 'amd64'

kernel_c_args = [ '-D_KERNEL' ]

if (arch == 'amd64')
	kernel_c_args += [ '-ffreestanding', '-fno-stack-protector', '-nostdlib',
	  '-fno-stack-check', '-fno-pic', '-fno-pie', '-mabi=sysv', '-mno-80387',
	  '-mno-mmx', '-mno-3dnow', '-mno-sse', '-mno-sse2', '-mno-red-zone',
	  '-mcmodel=kernel', '-D_KERNEL' ]
endif

kernel_objc_args = [ kernel_c_args, '--sysroot=/tmp/keyronex-sysroot' ]
kernel_includes = [ include_directories('kernel/' + arch, 'limine', 'kernel') ]

mlibc = subproject('mlibc', default_options: [ 'prefix=/usr', 'libdir=lib' ])
lai = subproject('lai')
libc_includes = mlibc.get_variable('libc_include_dirs')

bin2c = executable('bin2c', 'tools/bin2c.c', native: true)

executable('limine-deploy', 'limine/limine-deploy.c', native: true)

subdir('objfw/src')
subdir('kernel')
