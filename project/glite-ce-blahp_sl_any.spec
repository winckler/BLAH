Summary: The BLAHP daemon is a light component accepting commands to manage jobs on different Local Resources Management Systems
Name: glite-ce-blahp
Version:
Release:
License: Apache License 2.0
Vendor: EMI
Packager: CREAM group <cream-support@lists.infn.it>
URL: http://glite.cern.ch/
Group: Applications/Internet
BuildArch:
BuildRequires: libtool, classads-devel
Requires(post): chkconfig
Requires(preun): chkconfig
Requires(preun): initscripts
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)
AutoReqProv: yes
Source: %{name}-%{version}-%{release}.tar.gz

%global debug_package %{nil}

%description
The BLAHP daemon is a light component accepting commands to manage jobs on different Local Resources Management Systems

%prep
 

%setup -c

%build
%{!?extbuilddir:%define extbuilddir "--"}
if test "x%{extbuilddir}" == "x--" ; then
  ./configure --prefix=%{buildroot}/usr --sysconfdir=%{buildroot}/etc --disable-static PVER=%{version}
  make
fi

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}
%{!?extbuilddir:%define extbuilddir "--"}
if test "x%{extbuilddir}" == "x--" ; then
  make install
else
  cp -R %{extbuilddir}/* %{buildroot}
fi


%clean
rm -rf %{buildroot} 

%post

if [ $1 -eq 1 ] ; then
    /sbin/chkconfig --add glite-ce-blahparser

    if [ ! "x`grep tomcat /etc/passwd`" == "x" ] ; then
        mkdir -p /var/log/cream/accounting
        chown root.tomcat /var/log/cream/accounting
        chmod 0730 /var/log/cream/accounting
        
        mkdir -p /var/blahp
        chown tomcat.tomcat /var/blahp
        chmod 771 /var/blahp
    
    fi
fi

%preun
if [ $1 -eq 0 ] ; then
    /sbin/service glite-ce-blahparser stop >/dev/null 2>&1
    /sbin/chkconfig --del glite-ce-blahparser

    if [ -d /var/log/cream/accounting ] ; then
        rm -rf /var/log/cream/accounting 
    fi
    
    if [ -d /var/blahp ] ; then
        rm -rf /var/blahp
    fi
fi

%files
%defattr(-,root,root)
%config(noreplace) /etc/blparser.conf.template
%config(noreplace) /etc/blah.config.template
%dir /etc/rc.d/init.d/
/etc/rc.d/init.d/glite-ce-*
/usr/bin/*
%dir /usr/share/doc/glite-ce-blahp-%{version}/
%doc /usr/share/doc/glite-ce-blahp-%{version}/LICENSE

%changelog

