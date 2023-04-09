SUB=..
PKGNAME=coreutils
DISTFILES="https://ftp.gnu.org/gnu/coreutils/coreutils-9.2.tar.xz"

CONFSUBS="coreutils-9.2/build-aux/config.sub"
CONFIGURE_SCRIPT="coreutils-9.2/configure"

post-patch:
	cp ${KP_TOP}/vendor/config.sub ${PKG_WORKDIR}/coreutils-9.2/build-aux

.include "../buildsup/krx.gnu.mk"