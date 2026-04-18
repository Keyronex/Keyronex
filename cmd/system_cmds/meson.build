project('system_cmds', 'c')

libcrypt = dependency('libcrypt')

executable('getty',
    'getty.c',
    install: true,
    install_dir: get_option('sbindir'),
)

executable('login',
    'login.c',
    dependencies: libcrypt,
    install: true,
)
