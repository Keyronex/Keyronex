cat <<EOF > $2
[binaries]
c = 'x86_64-keyronex-gcc'
cpp = 'x86_64-keyronex-g++'
ar = 'x86_64-keyronex-ar'
strip = 'x86_64-keyronex-strip'
objc = 'x86_64-keyronex-gcc'
pkgconfig ='x86_64-keyronex-pkg-config'

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
