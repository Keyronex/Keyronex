project('Keyronex')

headers_only_option = get_option('headers_only')
no_headers_option = get_option('no_headers')
arch_option = get_option('arch')

if (target_machine.cpu_family() == 'x86_64')
    cpu = 'amd64'
else
    cpu = target_machine.cpu_family()
endif


if not headers_only_option
    add_languages('c', 'objc')

    limine = executable('limine', '../vendor/limine/limine.c', native: true)

    common_kernel_args = [
        '-ffreestanding',
        '-fno-stack-protector',
        '-fno-stack-check',
        '-fno-strict-aliasing',
        '-fno-omit-frame-pointer',
        '-DKRX_BUILDING_KERNEL',
        '-DUACPI_SIZED_FREES=1',
    ]

    common_kernel_objc_args = [
        '-std=gnu2x',
        '-fconstant-string-class=OSConstantString',
        '-DOF_OBJFW_RUNTIME=1',
        '-DOF_COMPILING_OBJFW=1',
    ]

    common_kernel_link_args = [
        '-nostdlib',
        '-Wl,-z,max-page-size=0x1000',
    ]

    if (target_machine.cpu_family() == 'x86_64')
        common_kernel_args += [
            '-mno-80387',
            '-mno-mmx',
            '-mno-sse',
            '-mno-sse2',
            '-m64',
            '-mabi=sysv',
            '-mno-red-zone',
        ]
        common_kernel_link_args += [
            '-mno-red-zone',
        ]

    elif (target_machine.cpu_family() == 'm68k')
        common_kernel_args += [ '-mcpu=68040' ]

    elif (target_machine.cpu_family() == 'aarch64')
        common_kernel_args += [
            '-mgeneral-regs-only'
        ]
        common_kernel_link_args += [
            '-march=rv64imac_zicsr_zifencei',
        ]

    elif (target_machine.cpu_family() == 'riscv64')
        common_kernel_args += [
            '-march=rv64imac_zicsr_zifencei',
            '-mabi=lp64',
            '-mno-relax',
        ]
        common_kernel_link_args += [
            '-march=rv64imac_zicsr_zifencei',
        ]

    else
        error('Unsupported CPU family')

    endif

endif # not headers_only_option

common_kernel_includes = [include_directories('../vendor')]
kernel_sources = []

subdir(cpu)
subdir('common')
