lwip = '../../vendor/lwip'

lwip_sources = files(
	lwip / 'core/ipv4/etharp.c', lwip / 'core/ipv4/icmp.c',
	lwip / 'core/ipv4/igmp.c', lwip / 'core/ipv4/ip4.c',
	lwip / 'core/ipv4/ip4_addr.c', lwip / 'core/ipv4/ip4_frag.c',

	lwip / 'core/ipv6/ethip6.c', lwip / 'core/ipv6/icmp6.c',
	lwip / 'core/ipv6/ip6.c', lwip / 'core/ipv6/ip6_addr.c',
	lwip / 'core/ipv6/ip6_frag.c', lwip / 'core/ipv6/mld6.c',
	lwip / 'core/ipv6/nd6.c',

	lwip / 'core/def.c', lwip / 'core/inet_chksum.c', lwip / 'core/init.c',
	lwip / 'core/ip.c', lwip / 'core/mem.c', lwip / 'core/memp.c',
	lwip / 'core/netif.c', lwip / 'core/pbuf.c', lwip / 'core/raw.c',
	lwip / 'core/stats.c', lwip / 'core/tcp_in.c', lwip / 'core/tcp_out.c',
	lwip / 'core/tcp.c', lwip / 'core/timeouts.c', lwip / 'core/udp.c',

	lwip / 'netif/ethernet.c',

	lwip / 'api/err.c',
)

lwip_includes = include_directories(lwip / 'include')

kernel_sources += lwip_sources
kernel_sources += files('buf.c', 'keysock_dev.m', 'net.c', 'sock_tcp.c',
	'util.c', 'sysdep.c')

freestanding_include_dirs += lwip_includes
