do-configure:
	mkdir -p ${PKG_WORKDIR}
	${KP_TOP}/buildsup/mkcrossfile.sh ${KP_SYSROOT} > ${PKG_WORKDIR}/cross-file.ini
	meson setup --cross-file=${PKG_WORKDIR}/cross-file.ini \
		--prefix=${PKG_PREFIX} \
		${MESON_ARGS} \
		${PKG_WORKDIR} \
		${PKG_SRCDIR}

do-build:
	ninja -C ${PKG_WORKDIR}

do-stage:
	DESTDIR=${PKG_STAGEDIR} ninja -C ${PKG_WORKDIR} install

.include "krx.pkg.mk"