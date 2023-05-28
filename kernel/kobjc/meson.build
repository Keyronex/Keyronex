kernel_sources += files('../../libdxf/dxf.c')

if (false)

kobjcrt_files = [ 'arc.m', 'autorelease.m', 'category.m',
	'class.m', 'dtable.m', 'exception.m', 'hashtable.m', 'init.m',
	'instance.m', 'ivar.m', 'lookup.m', 'method.m', 'misc.m', 'property.m',
	'protocol.m', 'selector.m', 'sparsearray.m', 'static-instances.m',
	'synchronized.m', 'tagged-pointer.m',

	'lookup-asm/lookup-asm.S',
	'../forwarding/forwarding.S',
]

kobjcrt_sources = []

foreach p : kobjcrt_files
	kobjcrt_sources += files('../../objfw/src/runtime/' + p)
endforeach

kobjcrt = static_library('kobjcrt', kobjcrt_sources,
		c_args: [ kernel_c_args ],
		objc_args: [ kernel_objc_args, '-DOBJC_COMPILING_RUNTIME',
				'-DOF_COMPILING_OBJFW', '-D_KERNEL' ],
		include_directories : [
			'.',
			'../../objfw/src',
			'../../objfw/src/runtime',
			krx_sdk_includes
		]
)

libkobjcrt_dep = declare_dependency(link_with : [ kobjcrt ],
		include_directories : [ '../../objfw/src/runtime'  ],
		compile_args: [ '-DOF_COMPILING_OBJFW' ]
)

kernel_sources += files('test.m')

else

libkobjcrt_dep = declare_dependency()

endif
