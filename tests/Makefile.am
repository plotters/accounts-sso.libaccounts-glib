## Process this file with automake to produce Makefile.in

INCLUDES=-I$(top_srcdir) $(LIBACCOUNTS_CFLAGS)

bin_PROGRAMS = accounts-glib-testsuite test-process 

bin_SCRIPTS = accounts-glib-test.sh
CLEANFILES = $(bin_SCRIPTS)

accounts_glib_testsuite_SOURCES = check_ag.c
accounts_glib_testsuite_CFLAGS = $(CHECK_FLAGS) -I$(top_srcdir)
accounts_glib_testsuite_LDADD = \
	$(CHECK_LIBS) \
	$(LIBACCOUNTS_LIBS) \
	$(top_builddir)/libaccounts-glib/libaccounts-glib.la

test_process_SOURCES = test-process.c
test_process_CFLAGS = -I$(top_srcdir)
test_process_LDADD = \
	$(LIBACCOUNTS_LIBS) \
	$(top_builddir)/libaccounts-glib/libaccounts-glib.la

TESTS_ENVIRONMENT = \
	ACCOUNTS=/tmp/ \
	AG_APPLICATIONS=$(top_srcdir)/tests/ \
	AG_SERVICES=$(top_srcdir)/tests/ \
	AG_SERVICE_TYPES=$(top_srcdir)/tests/ \
	AG_PROVIDERS=$(top_srcdir)/tests/ \
	AG_DEBUG=all \
	G_MESSAGES_DEBUG=all

TESTS = accounts-glib-testsuite

testdata_DATA = \
	tests.xml \
	e-mail.service-type \
	Gallery.application \
	Mailer.application \
	MyProvider.provider \
	MyService.service \
	MyService2.service \
	OtherService.service

EXTRA_DIST = \
	tests.xml \
	e-mail.service-type \
	Gallery.application \
	Mailer.application \
	MyProvider.provider \
	MyService.service \
	MyService2.service \
	OtherService.service

