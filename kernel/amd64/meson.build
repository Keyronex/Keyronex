ldscript = meson.current_source_dir()/'linker.ld'

kern_srcs += lai.get_variable('sources')
kernel_includes += [ lai.get_variable('includes') ]
kern_srcs += files( 'amd64.c', 'locore.S', 'start.c', 'vm_machdep.c' )

vxkern = executable('keyronex', kern_srcs,
	c_args : [ kernel_c_args ],
	objc_args: [ kernel_objc_args ],
	link_args : [ '-nostdlib', '-static',
	  '-Wl,-z,max-page-size=0x1000', '-Wl,--build-id=none',
	  '-Wl,-T' + ldscript ],
	include_directories: [ kernel_includes ],
	dependencies: [ libkobjc_dep ]
)
