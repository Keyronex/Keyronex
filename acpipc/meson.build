acpipc = static_library('acpipc', 'acpipc.cc', lai_sources,
	c_args: [ freestanding_c_args ],
	cpp_args: [ freestanding_cpp_args, '-fno-exceptions' ],
	include_directories: [ mlx_sdk_includes, lai_includes ],
	override_options : ['cpp_std=gnu++2a'])