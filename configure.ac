dnl Process this file with autoconf to produce a configure script.
AC_PREREQ([2.68])

AC_INIT([fiberstat], [0.7.0], [aleksander@aleksander.es])
AC_CONFIG_SRCDIR([config.h.in])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])

AM_INIT_AUTOMAKE([1.11 foreign no-define no-dist-gzip dist-xz tar-ustar -Wno-portability])
AM_MAINTAINER_MODE([enable])

dnl Support silent build rules. Disable
dnl by either passing --disable-silent-rules to configure or passing V=1
dnl to make
AM_SILENT_RULES([yes])

dnl Required programs
AC_PROG_CC
AM_PROG_CC_C_O
AC_PROG_INSTALL
AX_PTHREAD

dnl Initialize libtool
LT_PREREQ([2.2])
LT_INIT

dnl Specific warnings to always use
FIBERSTAT_COMPILER_WARNINGS

dnl ncurses
PKG_CHECK_MODULES([NCURSES], [ncurses])
AC_SUBST(NCURSES_CFLAGS)
AC_SUBST(NCURSES_LIBS)

AC_CONFIG_FILES([Makefile
                 src/Makefile
                 test/Makefile])
AC_OUTPUT

echo "
    fiberstat $VERSION
    ==============================================

    compiler:        ${CC}
    cflags:          ${CFLAGS}
    maintainer mode: ${USE_MAINTAINER_MODE}
"
