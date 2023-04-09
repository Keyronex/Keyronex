KP_ENV_FLAGS=SYSROOT=${KP_SYSROOT} \
  CC="x86_64-keyronex-gcc --sysroot=${KP_SYSROOT}" \
  CXX="x86_64-keyronex-g++ --sysroot=${KP_SYSROOT}"
  LD="x86_64-keyronex-ld --sysroot=${KP_SYSROOT}"

# changed to alter CC because at least ncurses doesn't respect CFLAGS

#CFLAGS="--sysroot=${KP_SYSROOT}" \
#  CPPFLAGS="--sysroot=${KP_SYSROOT}" \
#  LDFLAGS="--sysroot=${KP_SYSROOT}"

.if !target(do-configure)
do-configure:
	mkdir -p ${PKG_WORKDIR}/build
	(cd ${PKG_WORKDIR}/build && \
	  env ${KP_ENV_FLAGS} \
	  ${PKG_WORKDIR}/${CONFIGURE_SCRIPT} --prefix=${PKG_PREFIX} \
	  --host=x86_64-keyronex --with-sysroot=${KP_SYSROOT} ${CONFIGURE_ARGS})
.endif

.if !target(do-build)
do-build:
	(cd ${PKG_WORKDIR}/build && \
	  gmake -j 8)
.endif

.if !target(do-stage)
do-stage:
	(cd ${PKG_WORKDIR}/build && \
	  env ${KP_ENV_FLAGS} \
	  gmake install DESTDIR=${PKG_STAGEDIR})
.endif

.include "krx.pkg.mk"