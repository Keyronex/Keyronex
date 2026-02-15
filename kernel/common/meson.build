kernel_sources += [
    'devicekit/dk.m',

    'kern/callout.c',
    'kern/clock.c',
    'kern/disp_rt.c',
    'kern/disp_ts.c',
    'kern/dispatch.c',
    'kern/dlog.c',
    'kern/event.c',
    'kern/init.c',
    'kern/intr.c',
    'kern/kwait.c',
    'kern/mutex.c',
    'kern/rwlock.c',
    'kern/thread.c',
    'kern/turnstil.c',
    'kern/xcall.c',

    'libkern/objc/OSArray.m',
    'libkern/objc/OSDictionary.m',
    'libkern/objc/OSMapTable.m',
    'libkern/objc/OSNumber.m',
    'libkern/objc/OSObject.m',
    'libkern/objc/OSString.m',
    'libkern/murmurhash.c',
    'libkern/lib.c',

    'os/main.c',
    'os/proc.c',

    'vm/init.c',
    'vm/kmem.c',
    'vm/kwired.c',
    'vm/map.c',
    'vm/phys.c',
    'vm/pmap.c',
    'vm/vmem.c',
]

objfwrt_sources = files(
    '../../vendor/objfwrt/arc.m',
    '../../vendor/objfwrt/association.m',
    '../../vendor/objfwrt/autorelease.m',
    '../../vendor/objfwrt/category.m',
    '../../vendor/objfwrt/class.m',
    '../../vendor/objfwrt/dtable.m',
    '../../vendor/objfwrt/exception.m',
    '../../vendor/objfwrt/hashtable.m',
    '../../vendor/objfwrt/init.m',
    '../../vendor/objfwrt/instance.m',
    '../../vendor/objfwrt/ivar.m',
    '../../vendor/objfwrt/lookup.m',
    '../../vendor/objfwrt/method.m',
    '../../vendor/objfwrt/misc.m',
    '../../vendor/objfwrt/property.m',
    '../../vendor/objfwrt/protocol.m',
    '../../vendor/objfwrt/selector.m',
    '../../vendor/objfwrt/sparsearray.m',
    '../../vendor/objfwrt/static-instances.m',
    '../../vendor/objfwrt/synchronized.m',
    '../../vendor/objfwrt/tagged-pointer.m',
    '../../vendor/objfwrt/threading.m',
    '../../vendor/objfwrt/lookup-asm/lookup-asm.S',
)

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
        kernel_sources, objfwrt_sources,
        c_args: [common_kernel_args, kernel_binary_args],
        objc_args: [
            common_kernel_args,
            common_kernel_objc_args,
            kernel_binary_args
        ],
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
