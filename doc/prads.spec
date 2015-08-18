Name: prads
Version: 0.3.3
Summary: Passive Realtime Asset Detection System
Release: 1%{dist}
Group: Networking/Utilities
License: Perl license
Url: http://gamelinux.github.io/prads/
Source0: prads-0.3.3.tar.gz

BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)

BuildRequires: libpcap-devel
BuildRequires: pcre-devel
BuildRequires: python-docutils
Requires(pre): shadow-utils
Requires: libpcap
Requires: initscripts

# Automatic dependency addition. Change to no if you don't want a
# dependency on perl-XML-Writer (needed by prads2snort only)
AutoReqProv: yes

%description
PRADS is a passive real-time asset detection system.
It passively listen to network traffic and gathers information on hosts
and services it sees on the network. This information can be used to map
your network, letting you know what services and hosts are alive/used,
or can be used together with your favorite IDS/IPS setup for "event to
host/service" correlation.


%prep
%setup -q -n prads

# Patch the default make install script so that it doesn't break trying to
# set file owner/permissions when building the RPM as a non-root user.
sed -i 's/-o root -g [$]{INSTALLGROUP} //g' Makefile
sed -i 's/^PREFIX=.*/PREFIX=/' Makefile
sed -i 's/^BINDIR=.*/BINDIR=\/usr\/bin/' Makefile
sed -i 's/^MANDIR=.*/MANDIR=\/usr\/share\/man\/man1/' Makefile
sed -i 's/^INITDIR=.*/INITDIR=\/etc\/init.d/' Makefile

# Patch the sample config to use a dedicated user/group name
sed -i 's/^#user=.*/user=prads/'   etc/prads.conf
sed -i 's/^#group=.*/group=prads/' etc/prads.conf


%build
make man
make build

%install
%make_install

%clean

%pre
getent group prads > /dev/null || groupadd -r prads
getent passwd prads > /dev/null || \
	useradd -r -g prads -d /var/run/prads -s /sbin/nologin \
		-c "Passive Realtime Asset Detection System" prads


%post

%preun
if [[ $1 -eq 0 ]]
then
    /etc/init.d/prads stop > /dev/null 2>&1
    exit 0
fi

%postun

%files
%defattr(-,root,root)
%doc README changelog doc/*.txt
%doc doc/AUTHORS doc/ROADMAP doc/PRADS_LOGO.xcf doc/INSTALL doc/design
%doc doc/default-ttl doc/prads.sql doc/prads.dia doc/output-plugin.rst
/etc/prads/*.sig
/etc/prads/*.fp
/etc/prads/*.ports
/etc/prads/prads.conf
/etc/init.d/prads
/usr/bin/prads
/usr/bin/prads-asset-report
/usr/bin/prads2snort
/usr/share/man/man1/prads-asset-report.1.gz
/usr/share/man/man1/prads.1.gz
/usr/share/man/man1/prads2snort.1.gz

%changelog
