.include "krx.kp.mk"

PKG_SRCDIR?=${KP_SRCDIR}/${PKGNAME}
PKG_WORKDIR=${KP_WORKDIR}/${PKGNAME}
#PKG_STAGEDIR=${KP_STAGEDIR}/${PKGNAME}
PKG_STAGEDIR=${KP_SYSROOT}
PKG_PKGOUT=${KP_PKGOUTDIR}/${PKGNAME}.tar.gz

_PKG_FETCHDONE=${PKG_WORKDIR}/.fetch-done
_PKG_EXTRACTDONE=${PKG_WORKDIR}/.extract-done
_PKG_PATCHDONE=${PKG_WORKDIR}/.patch-done
_PKG_CONFDONE=${PKG_WORKDIR}/.conf-done
_PKG_BUILDDONE=${PKG_WORKDIR}/.build-done
_PKG_STAGEDONE=${PKG_WORKDIR}/.stage-done
_PKG_PACKAGEDONE=${PKG_WORKDIR}/.package-done

${_PKG_FETCHDONE}:
	@mkdir -p ${PKG_WORKDIR}
	@for url in ${DISTFILES}; do \
		if [ ! -f ${KP_DISTFILEDIR}/`basename $$url` ]; then \
			echo "==> Fetching $$url" ; \
			wget -P ${KP_DISTFILEDIR} $$url ; \
		fi ; \
	done ;
	touch ${_PKG_FETCHDONE}

${_PKG_EXTRACTDONE}: ${_PKG_FETCHDONE}
	@for url in ${DISTFILES}; do \
		echo "==> Extracting $$url" ; \
		tar -xf ${KP_DISTFILEDIR}/`basename $$url` -C ${PKG_WORKDIR} ; \
	done ;
	touch ${_PKG_EXTRACTDONE}

${_PKG_PATCHDONE}: ${_PKG_EXTRACTDONE}
	@if [ -d ${.CURDIR}/patches ]; then \
		for patch in ${.CURDIR}/patches/*; do \
			echo "==> Applying patch $$patch" ; \
			patch -p0 -d ${PKG_WORKDIR} < $$patch ; \
		done ; \
	fi ;

	# replace config.subs
	@for confsub in ${CONFSUBS}; do \
		echo "==> Replacing $$confsub" ; \
		cp ${KP_TOP}/vendor/config.sub ${PKG_WORKDIR}/$$confsub ; \
	done ;

	touch ${_PKG_PATCHDONE}

${_PKG_CONFDONE}: ${_PKG_PATCHDONE}
	@echo "==> Configuring ${PKGNAME}"
	@${MAKE} ${MAKEFLAGS} do-configure
	touch $@

${_PKG_BUILDDONE}: ${_PKG_CONFDONE}
	@echo "==> Building ${PKGNAME}"
	@${MAKE} ${MAKEFLAGS} do-build
	touch $@

${_PKG_STAGEDONE}: ${_PKG_BUILDDONE}
	@echo "==> Staging ${PKGNAME}"
	@${MAKE} ${MAKEFLAGS} do-stage
	touch $@

${_PKG_PACKAGEDONE} : ${_PKG_STAGEDONE}
	#mkdir -p ${KP_PKGOUTDIR}
	#tar -cf ${PKG_PKGOUT} -C ${PKG_STAGEDIR} ./

all: ${_PKG_PACKAGEDONE}