project('Keyronex')

headers_only = get_option('headers_only')
no_headers = get_option('no_headers')
port = get_option('arch')

if not headers_only
	add_languages('c', 'cpp', 'objc')
endif

if not no_headers
	subdir('kernel/include')
endif