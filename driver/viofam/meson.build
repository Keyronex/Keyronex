vio9pprt = static_library('vio9pprt', 'vio9pprt.cc', '9pfs.cc', '9pfs_reg.c',
	c_args: [ freestanding_c_args ],
	cpp_args: [ freestanding_cpp_args ],
	include_directories: [ krx_sdk_includes, lai_includes ])

viodisk = static_library('viodisk', 'viodev.cc', 'viodisk.cc',
	c_args: [ freestanding_c_args ],
	cpp_args: [ freestanding_cpp_args ],
	include_directories: [ krx_sdk_includes, lai_includes ])

viofsprt = static_library('viofsprt', 'viofsprt.cc', 'fusefs.cc',
	c_args: [ freestanding_c_args ],
	cpp_args: [ freestanding_cpp_args ],
	include_directories: [ krx_sdk_includes, lai_includes ])

vionet = static_library('vionet', 'vionet.cc',
	c_args: [ freestanding_c_args ],
	cpp_args: [ freestanding_cpp_args ],
	include_directories: [ krx_sdk_includes, lai_includes ])