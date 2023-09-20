kobjcrt_srcs = [ 'arc.m', 'autorelease.m', 'category.m',
	  'class.m', 'dtable.m', 'exception.m', 'hashtable.m', 'init.m',
	  'instance.m', 'ivar.m', 'lookup.m', 'method.m', 'misc.m', 'property.m',
	  'protocol.m', 'selector.m', 'sparsearray.m', 'static-instances.m',
	  'synchronized.m', 'tagged-pointer.m',
	  'lookup-asm/lookup-asm.S' ]

kobjcrt = static_library('kobjcrt', kobjcrt_srcs,
	c_args: [ freestanding_c_args ],
	objc_args: [ freestanding_c_args, '-DOBJC_COMPILING_RUNTIME',
	    '-DOF_COMPILING_OBJFW', '-std=gnu11'  ],
	link_args: [ '-lgcc' ],
	include_directories : [  '../../kernel/cshim', freestanding_include_dirs ] )

kobjcrt_dep = declare_dependency(link_with : [ kobjcrt ],
	include_directories : [ '.' ] )
