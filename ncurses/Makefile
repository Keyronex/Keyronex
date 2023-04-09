SUB=..
PKGNAME=ncurses
DISTFILES="https://ftp.gnu.org/gnu/ncurses/ncurses-6.4.tar.gz"

CONFSUBS="ncurses-6.4/config.sub"
CONFIGURE_SCRIPT="ncurses-6.4/configure"
CONFIGURE_ARGS+=--with-shared --with-termlib --with-ticlib --enable-widec \
  --enable-pc-files --with-pkg-config-libdir=/usr/lib/pkgconfig \
  --with-manpage-format=normal

do-post-stage:
	for lib in ncurses form panel menu tinfo ; do \
	   rm -vf                    "${PKG_STAGEDIR}${PKG_PREFIX}"/lib/lib$${lib}.so ; \
	   echo "INPUT(-l$${lib}w)" > "${PKG_STAGEDIR}${PKG_PREFIX}"/lib/lib$${lib}.so ; \
	   ln -sfv $${lib}w.pc        "${PKG_STAGEDIR}${PKG_PREFIX}"/lib/pkgconfig/$${lib}.pc ; \
	done

	rm -vf                     ${PKG_STAGEDIR}${PKG_PREFIX}/lib/libcursesw.so
	echo "INPUT(-lncursesw)" > ${PKG_STAGEDIR}${PKG_PREFIX}/lib/libcursesw.so
	ln -sfv libncurses.so      ${PKG_STAGEDIR}${PKG_PREFIX}/lib/libcurses.so

.include "../buildsup/krx.gnu.mk"