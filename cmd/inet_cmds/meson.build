project('inet_cmds', 'c')

executable('ifconfig',
    'ifconfig/ifconfig.c',
    'ifconfig/inet.c',
    'ifconfig/inet6.c',
    'ifconfig/netlink.c',
    install: true,
    install_dir: get_option('sbindir'),
)

executable('route', 'route.c',
    install: true,
    install_dir: get_option('sbindir'),
)
