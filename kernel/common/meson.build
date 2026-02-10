kernel_sources += [
    'kern/callout.c',
    'kern/disp_rt.c',
    'kern/disp_ts.c',
    'kern/dispatch.c',
    'kern/dlog.c',
    'kern/init.c',
    'kern/intr.c',
    'kern/kwait.c',
    'kern/turnstil.c',
    'kern/xcall.c',

    'libkern/lib.c',

    'os/main.c',

    'vm/init.c',
    'vm/phys.c',
    'vm/vmem.c',
]

if not headers_only_option
    kernel_binary_args = [
        '-fno-pie',
        '-fno-pic',
    ]

    kernel_binary_link_args = []

    if (target_machine.cpu_family() == 'x86_64')
        libgcc = get_option('libgcc_binaries')  / 'libgcc-x86_64-no-red-zone.a'
        kernel_binary_args += [
            '-mcmodel=kernel',
            '-mno-red-zone',
        ]
        kernel_binary_link_args += [
            '-mcmodel=kernel',
            '-mno-red-zone',
        ]

    elif (target_machine.cpu_family() == 'm68k')
        libgcc = get_option('libgcc_binaries')  / 'libgcc-m68k.a'

    elif (target_machine.cpu_family() == 'riscv64')
        libgcc = get_option('libgcc_binaries') / 'libgcc-riscv64-softfloat.a'

    elif (target_machine.cpu_family() == 'aarch64')
        libgcc = get_option('libgcc_binaries') / 'libgcc-aarch64.a'

    endif

    keyronex = executable(
        'keyronex',
        kernel_sources,
        c_args: [common_kernel_args, kernel_binary_args],
        include_directories: [common_kernel_includes ],
        link_args: [
            '-static',
            '-Wl,-T' + meson.current_source_dir() / '..' / cpu / 'linker.ld',
            common_kernel_link_args,
            kernel_binary_link_args,
            libgcc,
        ],
    )
endif # not headers_only_option

if not no_headers_option
    install_subdir('asm', install_dir: 'usr/include')
    install_subdir('linux', install_dir: 'usr/include')
    install_subdir('sys', install_dir: 'usr/include')
endif
