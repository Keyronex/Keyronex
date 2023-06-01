project('network_cmds', default_options: ['cpp_std=gnu++20'])

add_languages('objc')

objc = meson.get_compiler('objc')
dxf_dep = objc.find_library('dxf', required: true)
objfwrt_dep = objc.find_library('objfwrt', required: true)
objfw_dep = objc.find_library('objfw', required: true)

executable('ifconfig', 'ifconfig.m',
	objc_args: [ '-fconstant-string-class=OFConstantString', '-fgnu-runtime', '-std=gnu17'  ],
	dependencies: [ dxf_dep, objfwrt_dep, objfw_dep ],
	install: true
)
