PKG_SRCDIR?=${KP_SRCDIR}/${PKGNAME}
PKG_WORKDIR=${KP_WORKDIR}/${PKGNAME}
#PKG_STAGEDIR=${KP_STAGEDIR}/${PKGNAME}
PKG_STAGEDIR=${KP_SYSROOT}
PKG_PKGOUT=${KP_PKGOUTDIR}/${PKGNAME}.tar.gz

fetch: do-fetch

configure: do-configure

build: do-build

stage: do-stage

do-package:
	mkdir -p ${KP_PKGOUTDIR}
	tar -cf ${PKG_PKGOUT} -C ${PKG_STAGEDIR} ./

package: do-package

all: configure build stage package

.include "krx.kp.mk"