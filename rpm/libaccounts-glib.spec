Name:           libaccounts-glib
Version:        0.58
Release:        1
License:        LGPLv2.1
Summary:        Accounts base library
Url:            http://gitorious.org/accounts-sso/accounts-glib
Group:          System/Libraries
Source0:        %{name}-%{version}.tar.gz
BuildRequires:  automake
# In Tizen builds no gtk-doc support hence removing gtk-doc dependency
#BuildRequires:  gtk-doc
BuildRequires:  pkgconfig
BuildRequires:  pkgconfig(check) >= 0.9.4
BuildRequires:  pkgconfig(dbus-1)
BuildRequires:  pkgconfig(dbus-glib-1)
BuildRequires:  pkgconfig(glib-2.0) >= 2.30.0
BuildRequires:  pkgconfig(libxml-2.0)
BuildRequires:  pkgconfig(sqlite3)

%description
%{summary}.

%package devel
Summary:        Development files for %{name}
Group:          Development/Libraries
Requires:       %{name} = %{version}
Requires:       pkgconfig(glib-2.0)
Requires:       pkgconfig

%description devel
The %{name}-devel package contains libraries and header files for developing
applications that use %{name}.

%package tests
Summary:        Tests for %{name}
Group:          Development/Libraries
Requires:       %{name} = %{version}

%description tests
This package contains %{name} tests.

%prep
%setup -q

%build
# In Tizen builds no gtk-doc support hence removing gtk-doc dependency
#gtkdocize
autoreconf -vfi
%configure --disable-static --disable-gtk-doc
make %{?_smp_mflags}

%install
%make_install
%define srcdir %{_builddir}/%{name}-%{version}
install -p -m 644 %{srcdir}/tests/accounts-glib-test.sh $RPM_BUILD_ROOT/%{_bindir}/
mkdir -p $RPM_BUILD_ROOT/%{_datadir}/libaccounts-glib0-test/
install -p -m 644 %{srcdir}/tests/e-mail.service-type $RPM_BUILD_ROOT/%{_datadir}/libaccounts-glib0-test/
install -p -m 644 %{srcdir}/tests/MyProvider.provider $RPM_BUILD_ROOT/%{_datadir}/libaccounts-glib0-test/
install -p -m 644 %{srcdir}/tests/MyService.service $RPM_BUILD_ROOT/%{_datadir}/libaccounts-glib0-test/
install -p -m 644 %{srcdir}/tests/OtherService.service $RPM_BUILD_ROOT/%{_datadir}/libaccounts-glib0-test/
install -p -m 644 %{srcdir}/tests/tests.xml $RPM_BUILD_ROOT/%{_datadir}/libaccounts-glib0-test/


%post -p /sbin/ldconfig

%postun -p /sbin/ldconfig

%files
%defattr(-,root,root,-)
%doc README COPYING
%{_libdir}/libaccounts-glib.so.*
%{_datadir}/backup-framework/applications/accounts.conf
%{_bindir}/ag-backup
%{_bindir}/ag-tool

%files devel
%defattr(-,root,root,-)
%{_includedir}/libaccounts-glib/ag-types.h
%{_includedir}/libaccounts-glib/ag-account.h
%{_includedir}/libaccounts-glib/ag-errors.h
%{_includedir}/libaccounts-glib/ag-manager.h
%{_includedir}/libaccounts-glib/ag-provider.h
%{_includedir}/libaccounts-glib/ag-service-type.h
%{_includedir}/libaccounts-glib/ag-service.h
%{_includedir}/libaccounts-glib/ag-auth-data.h
%{_includedir}/libaccounts-glib/ag-application.h
%{_includedir}/libaccounts-glib/ag-account-service.h
%{_includedir}/libaccounts-glib/accounts-glib.h
%{_libdir}/libaccounts-glib.so
%{_libdir}/pkgconfig/libaccounts-glib.pc

%files tests
%defattr(-,root,root,-)
%{_bindir}/accounts-glib-test.sh
%{_bindir}/accounts-glib-testsuite
%{_bindir}/test-process
%{_datadir}/libaccounts-glib0-test/e-mail.service-type
%{_datadir}/libaccounts-glib0-test/MyProvider.provider
%{_datadir}/libaccounts-glib0-test/MyService.service
%{_datadir}/libaccounts-glib0-test/OtherService.service
%{_datadir}/libaccounts-glib0-test/tests.xml
%exclude %{_prefix}/doc/reference
