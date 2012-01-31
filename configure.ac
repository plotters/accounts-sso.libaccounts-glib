dnl Process this file with autoconf to produce a configure script.

AC_INIT(libaccounts-glib, 1.0)

AM_INIT_AUTOMAKE(AC_PACKAGE_NAME, AC_PACKAGE_VERSION)
AM_CONFIG_HEADER(config.h)

# Gobject Introspection
AC_CONFIG_MACRO_DIR([m4])
GOBJECT_INTROSPECTION_CHECK([1.30.0])

PKG_CHECK_MODULES(
	LIBACCOUNTS,
	dbus-1
	dbus-glib-1
	glib-2.0 >= 2.26
	gio-2.0 >= 2.30
	gio-unix-2.0
	gobject-2.0
	libxml-2.0
	sqlite3 >= 3.7.0);
AC_SUBST(LIBACCOUNTS_CFLAGS)
AC_SUBST(LIBACCOUNTS_LIBS)

PKG_CHECK_MODULES([CHECK], [check >= 0.9.4])

AC_ISC_POSIX
AC_PROG_CC
AM_PROG_CC_STDC
AC_HEADER_STDC
AC_PROG_LIBTOOL

GTK_DOC_CHECK([1.14], [--flavour no-tmpl])

DATABASE_DIR=".accounts"
AC_SUBST(DATABASE_DIR)
AC_DEFINE_UNQUOTED(DATABASE_DIR, "$DATABASE_DIR",
		   [Directory for storing database])

APPLICATION_FILES_DIR="accounts/applications"
AC_SUBST(APPLICATION_FILES_DIR)
AC_DEFINE_UNQUOTED(APPLICATION_FILES_DIR, "$APPLICATION_FILES_DIR",
		   [Directory for storing application definition files])

SERVICE_TYPE_FILES_DIR="accounts/service_types"
AC_SUBST(SERVICE_TYPE_FILES_DIR)
AC_DEFINE_UNQUOTED(SERVICE_TYPE_FILES_DIR, "$SERVICE_TYPE_FILES_DIR",
		   [Directory for storing service type files])

SERVICE_FILES_DIR="accounts/services"
AC_SUBST(SERVICE_FILES_DIR)
AC_DEFINE_UNQUOTED(SERVICE_FILES_DIR, "$SERVICE_FILES_DIR",
		   [Directory for storing service files])

PROVIDER_FILES_DIR="accounts/providers"
AC_SUBST(PROVIDER_FILES_DIR)
AC_DEFINE_UNQUOTED(PROVIDER_FILES_DIR, "$PROVIDER_FILES_DIR",
		   [Directory for storing provider files])

AC_ARG_ENABLE(cast-checks,  [  --disable-cast-checks   compile with GLIB cast checks disabled],[cchecks=${enableval}],cchecks=yes)
if test "x$cchecks" = "xno"; then
    CFLAGS="$CFLAGS -DG_DISABLE_CAST_CHECKS"
fi

AC_ARG_ENABLE(asserts,      [  --disable-asserts       compile with GLIB assertions disabled],[asserts=${enableval}],asserts=yes)
if test "x$asserts" = "xno"; then
    CFLAGS="$CFLAGS -DG_DISABLE_ASSERTS"
fi

AC_ARG_ENABLE(checks,       [  --disable-checks        compile with GLIB checks disabled],[checks=${enableval}],checks=yes) 
if test "x$checks" = "xno"; then
    CFLAGS="$CFLAGS -DG_DISABLE_CHECKS"
fi

AC_ARG_ENABLE(debug,     [  --enable-debug          enable printing of debug messages],[ddebug=${enableval}],ddebug=no)
if test "x$ddebug" = "xyes"; then
    CFLAGS="$CFLAGS -DENABLE_DEBUG"
fi

AC_ARG_ENABLE(coverage, [  --enable-coverage      compile with coverage info],[coverage=${enableval}],coverage=no)
if test "x$coverage" = "xyes"; then
    CFLAGS="$CFLAGS -g -fprofile-arcs -ftest-coverage"
fi

testdatadir=${datadir}/libaccounts-glib0-test
AC_SUBST(testdatadir)


# Python support

AM_PATH_PYTHON

PYGOBJECT_REQUIRED=2.90

AC_ARG_ENABLE([python],
              AS_HELP_STRING([--enable-python[=@<:@no/auto/yes@:>@]],[Build with python support]),
              [enable_python=$enableval],
              [enable_python="auto"])

if test "x$enable_python" = "xauto"; then
    PKG_CHECK_EXISTS([pygobject-3.0 >= $PYGOBJECT_REQUIRED],
                    [enable_python=yes],[enable_python=no])
fi

if test "x$enable_python" = "xyes"; then
    PKG_CHECK_MODULES(PYTHON, [pygobject-3.0 >= $PYGOBJECT_REQUIRED])

    pyoverridesdir=`$PYTHON -c "import gi;print gi._overridesdir"`
    AC_SUBST(pyoverridesdir)
fi

AM_CONDITIONAL(ENABLE_PYTHON, test x"$enable_python" = "xyes")


AC_CONFIG_FILES([tests/accounts-glib-test.sh], [chmod +x tests/accounts-glib-test.sh])

AC_OUTPUT([
	Makefile
	libaccounts-glib/Makefile
	libaccounts-glib.pc
	docs/Makefile
	docs/reference/Makefile
	pygobject/Makefile
	tests/Makefile
	tools/Makefile
])
