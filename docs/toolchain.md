Building the Toolchain
----------------------

Goes something like this (maybe a bit different, I don't remember:)

```
export TARGET=x86_64-scalux
export PREFIX=/opt/scalux
export SYSROOT=/tmp/scalux
export PATH=/opt/scalux/bin:$PATH

wget https://ftp.gnu.org/gnu/binutils/binutils-2.38.tar.xz
wget https://ftp.gnu.org/gnu/gcc/gcc-12.1.0/gcc-12.1.0.tar.xz

tar xvf binutils-2.38.tar.xz
tar xvf gcc-12.1.0.tar.xz

cd binutils-2.38
patch -p1 -i < ../binutils.diff

cd gcc-12.1.0
patch -p1 -i < ../gcc.diff
cd libstdc++-v3
autoconf

mkdir ../../bld-binutils
cd ../../bld-binutils
../binutils-2.38/configure --prefix="$PREFIX" --target="$TARGET" --with-sysroot="$SYSROOT" --disable-werror --enable-targets=x86_64-elf,x86_64-pe
make
make install

cd ..
mkdir build-mlibc1
cd build-mlibc1
meson --cross-file=../scalux-amd64.ini --prefix=/usr -Dheaders_only=true
ninja
DESTDIR=/tmp/scalux ninja install

mkdir ../bld-gcc
cd ../bld-gcc

./gcc-12.1.0-sc/configure --prefix="$PREFIX" --target="$TARGET" --with-sysroot="$SYSROOT" --enable-languages=c,c++,objc --disable-multilib --enable-initfini-array --disable-nls
make all-gcc
make install-gcc

mkdir ../bld-mlibc2
cd ../bld-mlibc2
meson --cross-file ../../scalux-gcc-x64.ini  --prefix=/usr --libdir=lib ../../mlibc
ninja
ninja install

cd ../bld-gcc
make all-target-libgcc
make install-target-libgcc
make all-target-libstdc++-v3
make install-target-libstdc++-v3
```
