project('network_cmds', default_options: ['cpp_std=gnu++20'])

add_languages('c', 'objc')

objc = meson.get_compiler('objc')
dxf_dep = objc.find_library('dxf', required: true)
objfwrt_dep = objc.find_library('objfwrt', required: true)
objfw_dep = objc.find_library('objfw', required: true)

executable('ifconfig', 'ifconfig.m',
	objc_args: [ '-fconstant-string-class=OFConstantString', '-fgnu-runtime', '-std=gnu17'  ],
	dependencies: [ dxf_dep, objfwrt_dep, objfw_dep ],
	install: true
)

#
# udhcpc
#

udhcpc_sources = []
foreach p : [ 'clientpacket.c', 'dhcpc.c', 'options.c', 'pidfile.c', 'packet.c',
	'script.c', 'socket.c',]
	udhcpc_sources += files('../vendor/udhcp/' + p)
endforeach

udhcpc = executable('udhcpc', udhcpc_sources,
	include_directories : [
		'../vendor/udhcp',
	],
	c_args: [ '-DVERSION="0.9.8-keyronex"'],
	install: true
)

install_data('default.script',
	install_dir: 'share/udhcpc'
)
