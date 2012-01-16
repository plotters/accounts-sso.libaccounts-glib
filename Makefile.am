SUBDIRS = libaccounts-glib docs tests tools
if ENABLE_PYTHON
SUBDIRS += pygobject
endif

ACLOCAL_AMFLAGS = -I m4

DISTCHECK_CONFIGURE_FLAGS = \
	--enable-gtk-doc \
	--enable-introspection=yes

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = libaccounts-glib.pc

libaccountsdocdir = ${prefix}/doc/reference
libaccountsdoc_DATA= README COPYING AUTHORS ChangeLog INSTALL NEWS

backupconfdir = ${prefix}/share/backup-framework/applications
backupconf_in_files = accounts.conf.in 
backupconf_DATA= accounts.conf

CLEANFILES = \
	$(backupconf_DATA)

EXTRA_DIST = \
	$(backupconf_in_files) \
	$(libaccountsdoc_DATA) \
	m4/introspection.m4

$(backupconf_DATA): $(backupconf_in_files) 
	@sed -e "s|\@DATABASE_DIR\@|$(DATABASE_DIR)|" $< > $@ 
