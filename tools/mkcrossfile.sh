ARCH=$1
SYSROOT=$2
FILE=$3

if [ "$ARCH" = "amd64" ]; then
  TRIPLE=x86_64-keyronex
  cpu_family=x86_64
  endian=little
  cpu=i686
elif [ "$ARCH" = "m68k" ]; then
  TRIPLE=m68k-keyronex
  cpu_family=m68k
  endian=big
  cpu=m68k
elif [ "$ARCH" = "aarch64" ]; then
  TRIPLE=aarch64-keyronex
  cpu_family=aarch64
  endian=little
  cpu=aarch64
else
  echo "Unsupported architecture: $ARCH"
  exit 1
fi

cat <<EOF > $FILE
[binaries]
as = '$TRIPLE-as'
c = '$TRIPLE-gcc'
cpp = '$TRIPLE-g++'
ar = '$TRIPLE-ar'
strip = '$TRIPLE-strip'
objc = '$TRIPLE-gcc'
pkgconfig ='$TRIPLE-pkg-config'

[host_machine]
system = 'keyronex'
cpu_family = '$cpu_family'
endian = '$endian'
cpu = '$cpu'

[built-in options]
c_args = '--sysroot=$SYSROOT'
cpp_args = '--sysroot=$SYSROOT'
objc_args = '--sysroot=$SYSROOT'
EOF
