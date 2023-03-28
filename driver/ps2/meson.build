ps2 = static_library('ps2', 'ps2kb.cc',
	c_args: [ freestanding_c_args ],
	cpp_args: [ freestanding_cpp_args, '-fno-exceptions' ],
	include_directories: [ krx_sdk_includes, lai_includes ])

drivers += ps2