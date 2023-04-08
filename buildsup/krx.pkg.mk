KP_TOP=${.CURDIR}/${SUB}
KP_WORKDIR?=/tmp/keybld
KP_SYSROOT?=${KP_WORKDIR}/sysroot
KP_STAGEDIR?=${KP_WORKDIR}/staging
KP_PKGOUTDIR?=${KP_WORKDIR}/packages

PKG_WORKDIR=${KP_WORKDIR}/${PKGNAME}
PKG_STAGEDIR=${KP_STAGEDIR}/${PKGNAME}
PKG_PKGOUT=${KP_PKGOUTDIR}/${PKGNAME}.tar.gz


do-package:
	mkdir -p ${KP_PKGOUTDIR}
	tar -cf ${PKG_PKGOUT} -C ${PKG_STAGEDIR} ./

all: do-configure do-build do-stage do-package 