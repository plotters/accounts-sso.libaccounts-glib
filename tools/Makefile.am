## Process this file with automake to produce Makefile.in

INCLUDES=-I$(top_srcdir) $(LIBACCOUNTS_CFLAGS)

bin_PROGRAMS = \
    ag-backup \
    ag-tool

CLEANFILES = $(BUILD_DIR)

ag_tool_SOURCES = main.c
ag_tool_CFLAGS = $(CHECK_FLAGS) -I$(top_srcdir)
ag_tool_LDADD = \
        $(CHECK_LIBS) \
	    $(LIBACCOUNTS_LIBS) \
        $(top_builddir)/libaccounts-glib/libaccounts-glib.la

ag_backup_SOURCES = backup.c
ag_backup_LDADD = \
	$(LIBACCOUNTS_LIBS)
