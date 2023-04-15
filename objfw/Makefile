SUB=..
PKGNAME=libobjfw
PKG_SRCDIR=${.CURDIR}/src

NEED_AUTOGEN=yes
CONFIGURE_SCRIPT=configure
CONFIGURE_ARGS+=--sysconfdir=/etc --localstatedir=/var --without-tls
# ObjFW doesn't build out-of-tree
PKG_GNU_BUILD_DIR=${PKG_SRCDIR}

.include "../buildsup/krx.gnu.mk"