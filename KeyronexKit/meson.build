project('KeyronexKit', 'c', 'objc')

objc = meson.get_compiler('objc')
dxf_dep = objc.find_library('dxf', required: true)
objfwrt_dep = objc.find_library('objfwrt', required: true)
objfw_dep = objc.find_library('objfw', required: true)

shared_library('keyronex',
	'KXInvocation/invoke-amd64.s', 'KXInvocation/KXInvocation.m',
	'KXInvocation/runtime.m',

	'KXIPC/KXEncoder.m', 'KXIPC/KXDecoder.m', 'KXIPC/KXIPCConnection.m',
	'KXIPC/KXIPCListener.m', 'KXIPC/KXIPCRemoteObject.m',

	'KXIPC/OFString+KXEncoding.m', 'KXIPC/KXInvocation+KXEncoding.m',

	objc_args: [ '-fconstant-string-class=OFConstantString', '-fgnu-runtime', '-std=gnu17'  ],
	dependencies: [ dxf_dep, objfwrt_dep, objfw_dep ],
	include_directories: '.',
	install: true
)
