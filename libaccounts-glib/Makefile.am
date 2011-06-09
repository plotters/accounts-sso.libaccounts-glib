lib_LTLIBRARIES = \
	libaccounts-glib.la

libaccounts_glib_la_CFLAGS = $(LIBACCOUNTS_CFLAGS) $(AEGISCRYPTO_CFLAGS) -Wall -Werror
libaccounts_glib_la_LIBADD = $(LIBACCOUNTS_LIBS) $(AEGISCRYPTO_LIBS) -lrt
libaccounts_glib_la_LDFLAGS = \
	-version-info 1:3:1 \
	-export-symbols-regex '^ag_'

BUILT_SOURCES = \
	ag-marshal.c \
	ag-marshal.h

CLEANFILES = $(BUILT_SOURCES)

libaccounts_glib_la_SOURCES = \
	ag-account.h \
	ag-account.c \
	ag-debug.h \
	ag-debug.c \
	ag-errors.h \
	ag-internals.h \
	ag-manager.h \
	ag-manager.c \
	ag-provider.h \
	ag-provider.c \
	ag-service.h \
	ag-service.c \
	ag-service-type.h \
	ag-service-type.c \
	ag-util.h \
	ag-util.c \
	$(BUILT_SOURCES)

libaccounts_glib_includedir = $(includedir)/libaccounts-glib
libaccounts_glib_include_HEADERS = \
	ag-account.h \
	ag-errors.h \
	ag-provider.h \
	ag-service.h \
	ag-service-type.h \
	ag-manager.h

%-marshal.h: %-marshal.list Makefile
	glib-genmarshal --header --prefix=$(subst -,_,$*)_marshal $< > $*-marshal.h

%-marshal.c: %-marshal.list Makefile
	glib-genmarshal --body --prefix=$(subst -,_,$*)_marshal $< > $*-marshal.c


EXTRA_DIST = ag-marshal.list

