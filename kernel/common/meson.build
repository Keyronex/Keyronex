kernel_sources += [
    'devicekit/virtio/DKVirtIOTransport.m',
    'devicekit/virtio/VirtIO9pPort.m',
    'devicekit/virtio/VirtIOMMIOTransport.m',

    'devicekit/SimpleFramebuffer.m',

    'devicekit/DKAxis.m',
    'devicekit/DKDevice.m',
    'devicekit/DKPROMNode.m',
    'devicekit/dk.m',

    'fs/9p/9pbuf.c',
    'fs/9p/9p_vnops.c',
    'fs/devfs/devfs.c',
    'fs/fifofs.c',
    'fs/namecache.c',
    'fs/sockfs.c',
    'fs/syscalls.c',
    'fs/viewcache.c',
    'fs/vnode.c',

    'io/console.c',
    'io/epoll.c',
    'io/iop.c',
    'io/strhead.c',
    'io/strsched.c',
    'io/strsubr.c',
    'io/ttyld.c',
    'io/unix.c',

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
    'libkern/idalloc.c',
    'libkern/murmurhash.c',
    'libkern/lib.c',

    'os/copyinout.c',
    'os/cred.c',
    'os/exec.c',
    'os/file.c',
    'os/filedesc.c',
    'os/futex.c',
    'os/jobctl.c',
    'os/main.c',
    'os/proc.c',
    'os/signal.c',
    'os/syscall.c',

    'vm/init.c',
    'vm/fault.c',
    'vm/fork.c',
    'vm/kmem.c',
    'vm/kwired.c',
    'vm/map.c',
    'vm/mmap.c',
    'vm/obj.c',
    'vm/phys.c',
    'vm/pmap.c',
    'vm/rs.c',
    'vm/vc_support.c',
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

uacpi_sources = files(
    '../../vendor/uacpi/source/default_handlers.c',
    '../../vendor/uacpi/source/event.c',
    '../../vendor/uacpi/source/interpreter.c',
    '../../vendor/uacpi/source/io.c',
    '../../vendor/uacpi/source/mutex.c',
    '../../vendor/uacpi/source/namespace.c',
    '../../vendor/uacpi/source/notify.c',
    '../../vendor/uacpi/source/opcodes.c',
    '../../vendor/uacpi/source/opregion.c',
    '../../vendor/uacpi/source/osi.c',
    '../../vendor/uacpi/source/registers.c',
    '../../vendor/uacpi/source/resources.c',
    '../../vendor/uacpi/source/shareable.c',
    '../../vendor/uacpi/source/sleep.c',
    '../../vendor/uacpi/source/stdlib.c',
    '../../vendor/uacpi/source/tables.c',
    '../../vendor/uacpi/source/types.c',
    '../../vendor/uacpi/source/uacpi.c',
    '../../vendor/uacpi/source/utilities.c',
)

if target_machine.cpu_family() != 'm68k'
    kernel_sources += [ uacpi_sources ]
    kernel_sources += files(
        'devicekit/acpi/DKACPINode.m',
        'devicekit/acpi/DKACPIPlatformRoot.m',
        'devicekit/acpi/uacpi.c',
        'devicekit/pci/DKPCIBridge.m',
        'devicekit/pci/DKPCIDevice.m',
        'devicekit/pci/pci.c',
        'devicekit/virtio/VirtIOPCITransport.m',
        'devicekit/PS2Keyboard.m',
    )
    common_kernel_includes += include_directories('../../vendor/uacpi/include')
endif


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
