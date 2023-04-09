cat <<EOF
[binaries]
c = '/opt/x86_64-keyronex/bin/x86_64-keyronex-gcc'
cpp = '/opt/x86_64-keyronex/bin/x86_64-keyronex-g++'
ar = '/opt/x86_64-keyronex/bin/x86_64-keyronex-ar'
strip = '/opt/x86_64-keyronex/bin/x86_64-keyronex-strip'
objc = 'clang'

[host_machine]
system = 'keyronex'
cpu_family = 'x86_64'
endian = 'little'
cpu = 'i686'

[built-in options]
c_args = '--sysroot=$1'
cpp_args = '--sysroot=$1'
objc_args = '--sysroot=$1'
EOF
