kern_srcs = files(
	'dev/ACPIPC.m', 'dev/autoconf.m', 'dev/IOAPIC.m', 'dev/laiex.c',
	'dev/PCIBus.m', 'dev/VirtIOFamily/VirtIOBlockDevice.m',
	'dev/VirtIOFamily/VirtIODevice.m', 'dev/VirtIOFamily/VirtIONetwork.m',
	'dev/dev.c',

	'devicekit/DKDevice.m',

    'nanokern/clock.c', 'nanokern/msgqueue.c', 'nanokern/mutex.c',
	'nanokern/semaphore.c', 'nanokern/thread.c', 'nanokern/timer.c',
	'nanokern/wait.c',

    'kern/kmem_slab.c', 'kern/kmem_liballoc.c', 'kern/liballoc_sysdep.c',
	'kern/ksrv.c', 'kern/kstart.c', 'kern/obj.c', 'kern/vmem.c',

    'libkern/libkern.c',

	'net/lwip_sup.c',

	'tmpfs/tmp_vfsops.c',
	'tmpfs/tmpfs.c',
	'tmpfs/tmpfs.h',

	'vfs/vfs.c',

    'vm/vm_kernel.c', 'vm/vm_page.c', 'vm/vm_pdaemon.c', 'vm/vm.c'
)

kern_srcs += files (
	'net/lwip/api/api_lib.c',
	'net/lwip/api/api_msg.c',
	'net/lwip/api/err.c',
	'net/lwip/api/if_api.c',
	'net/lwip/api/netbuf.c',
	'net/lwip/api/netdb.c',
	'net/lwip/api/netifapi.c',
	'net/lwip/api/sockets.c',
	'net/lwip/api/tcpip.c',


	'net/lwip/netif/bridgeif.c',
	'net/lwip/netif/bridgeif_fdb.c',
	'net/lwip/netif/ethernet.c',
	'net/lwip/netif/lowpan6.c',
	'net/lwip/netif/lowpan6_ble.c',
	'net/lwip/netif/lowpan6_common.c',
	'net/lwip/netif/ppp/auth.c',
	'net/lwip/netif/ppp/ccp.c',
	'net/lwip/netif/ppp/chap-md5.c',
	'net/lwip/netif/ppp/chap-new.c',
	'net/lwip/netif/ppp/chap_ms.c',
	'net/lwip/netif/ppp/demand.c',
	'net/lwip/netif/ppp/eap.c',
	'net/lwip/netif/ppp/ecp.c',
	'net/lwip/netif/ppp/eui64.c',
	'net/lwip/netif/ppp/fsm.c',
	'net/lwip/netif/ppp/ipcp.c',
	'net/lwip/netif/ppp/ipv6cp.c',
	'net/lwip/netif/ppp/lcp.c',
	'net/lwip/netif/ppp/magic.c',
	'net/lwip/netif/ppp/mppe.c',
	'net/lwip/netif/ppp/multilink.c',
	'net/lwip/netif/ppp/polarssl/arc4.c',
	'net/lwip/netif/ppp/polarssl/des.c',
	'net/lwip/netif/ppp/polarssl/md4.c',
	'net/lwip/netif/ppp/polarssl/md5.c',
	'net/lwip/netif/ppp/polarssl/sha1.c',
	'net/lwip/netif/ppp/ppp.c',
	'net/lwip/netif/ppp/pppapi.c',
	'net/lwip/netif/ppp/pppcrypt.c',
	'net/lwip/netif/ppp/pppoe.c',
	'net/lwip/netif/ppp/pppol2tp.c',
	'net/lwip/netif/ppp/pppos.c',
	'net/lwip/netif/ppp/upap.c',
	'net/lwip/netif/ppp/utils.c',
	'net/lwip/netif/ppp/vj.c',
#	'net/lwip/netif/slipif.c',
	'net/lwip/netif/zepif.c',
	'net/lwip/core/altcp.c',
	'net/lwip/core/altcp_alloc.c',
	'net/lwip/core/altcp_tcp.c',
	'net/lwip/core/def.c',
	'net/lwip/core/dns.c',
	'net/lwip/core/inet_chksum.c',
	'net/lwip/core/init.c',
	'net/lwip/core/ip.c',
	'net/lwip/core/ipv4/autoip.c',
	'net/lwip/core/ipv4/dhcp.c',
	'net/lwip/core/ipv4/etharp.c',
	'net/lwip/core/ipv4/icmp.c',
	'net/lwip/core/ipv4/igmp.c',
	'net/lwip/core/ipv4/ip4.c',
	'net/lwip/core/ipv4/ip4_addr.c',
	'net/lwip/core/ipv4/ip4_frag.c',
	'net/lwip/core/ipv6/dhcp6.c',
	'net/lwip/core/ipv6/ethip6.c',
	'net/lwip/core/ipv6/icmp6.c',
	'net/lwip/core/ipv6/inet6.c',
	'net/lwip/core/ipv6/ip6.c',
	'net/lwip/core/ipv6/ip6_addr.c',
	'net/lwip/core/ipv6/ip6_frag.c',
	'net/lwip/core/ipv6/mld6.c',
	'net/lwip/core/ipv6/nd6.c',
	'net/lwip/core/mem.c',
	'net/lwip/core/memp.c',
	'net/lwip/core/netif.c',
	'net/lwip/core/pbuf.c',
	'net/lwip/core/raw.c',
	'net/lwip/core/stats.c',
	'net/lwip/core/sys.c',
	'net/lwip/core/tcp.c',
	'net/lwip/core/tcp_in.c',
	'net/lwip/core/tcp_out.c',
	'net/lwip/core/timeouts.c',
	'net/lwip/core/udp.c'
	)

gen = generator(bin2c,
    output : '@PLAINNAME@.c',
    arguments : [ '-i', '@INPUT@', '-o', '@OUTPUT@', '-s', '@BASENAME@'  ]
)

kern_srcs += gen.process(['keynex.data'])

kernel_includes += [ include_directories('net/lwip/include') ]

subdir(arch)
