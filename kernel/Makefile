SUB=".."

PKGNAME=kernel
PKG_SRCDIR=./

do-configure:
	meson setup --cross-file=${KP_TOP}/buildsup/keyronex-amd64.ini \
		--prefix=/ \
		${PKG_WORKDIR} \
		${PKG_SRCDIR}

do-build:
	ninja -C ${PKG_WORKDIR}

do-stage:
	DESTDIR=${PKG_STAGEDIR} ninja -C ${PKG_WORKDIR} install


.include "../buildsup/krx.pkg.mk"