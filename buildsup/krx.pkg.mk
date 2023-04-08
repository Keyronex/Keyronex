KP_TOP=${.CURDIR}/${SUB}
KP_WORKDIR?=/tmp/keybld
KP_SRCDIR?=${KP_WORKDIR}/source
KP_SYSROOT?=${KP_WORKDIR}/sysroot
KP_STAGEDIR?=${KP_WORKDIR}/staging
KP_PKGOUTDIR?=${KP_WORKDIR}/packages

PKG_SRCDIR?=${KP_SRCDIR}/${PKGNAME}
PKG_WORKDIR=${KP_WORKDIR}/${PKGNAME}
PKG_STAGEDIR=${KP_STAGEDIR}/${PKGNAME}
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