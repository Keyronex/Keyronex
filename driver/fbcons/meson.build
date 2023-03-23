fbcons = static_library('fbcons',
	'../../vendor/limine-terminal/term.c',
	'../../vendor/limine-terminal/backends/framebuffer.c',
	'fbcons.cc', bin2c_gen.process('key272x70.data'),
	bin2c_gen.process('netascale102x82.data'),
	c_args: [ freestanding_c_args ],
	cpp_args: [ freestanding_cpp_args, '-fno-exceptions' ],
	include_directories: [ krx_sdk_includes, lai_includes ])