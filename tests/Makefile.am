## Process this file with automake to produce Makefile.in

INCLUDES=-I$(top_srcdir) $(LIBACCOUNTS_CFLAGS)

check_PROGRAMS = accounts-glib-testsuite test-process 

check_SCRIPTS = accounts-glib-test.sh

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
	TESTDATADIR=$(top_srcdir)/tests/

TESTS = accounts-glib-test.sh

EXTRA_DIST = \
	tests.xml \
	e-mail.service-type \
	Gallery.application \
	Mailer.application \
	MyProvider.provider \
	MyService.service \
	MyService2.service \
	OtherService.service

