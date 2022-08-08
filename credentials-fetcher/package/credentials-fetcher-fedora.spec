%global major_version 0
%global minor_version 0
%global patch_version 91

# Set to RC version if building RC, else %%{nil}
%global rcsuf rc2
%{?rcsuf:%global relsuf .%{rcsuf}}
%{?rcsuf:%global versuf -%{rcsuf}}

# For handling bump release by rpmdev-bumpspec and mass rebuild
%global baserelease 0.2

Name:           credentials-fetcher
Version:        %{major_version}.%{minor_version}.%{patch_version}
Release:        %{baserelease}%{?relsuf}%{?dist}
Summary:        credentials-fetcher is a daemon that refreshes tickets or tokens periodically

License:        Apache 2.0
URL:            tbd-project.com
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake3 make chrpath

Requires: bind-utils openldap mono-core

%description
This daemon creates and refreshes kerberos tickets, these tickets can be
used to launch new containers.
The gMSA feature can be implemented using this daemon.
Kerberos tickets are refreshed when tickets expire or when a gMSA password changes.
The same method can be used to refresh other types of security tokens.
This spec file is specific to Fedora, use this file to rpmbuild on Fedora.


# https://docs.fedoraproject.org/en-US/packaging-guidelines/CMake/
%prep
%setup -q

%build
%cmake3
%cmake_build

%install

%cmake_install
# https://docs.fedoraproject.org/en-US/packaging-guidelines/#_removing_rpath
# https://docs.fedoraproject.org/en-US/packaging-guidelines/#_rpath_for_internal_libraries
chrpath --delete %{buildroot}/opt/credentials-fetcher/bin/credentials-fetcherd

%check
# TBD: Run tests from top-level directory
ctest3

%files
/opt/credentials-fetcher/bin/credentials-fetcherd
# https://tldp.org/LDP/Linux-Filesystem-Hierarchy/html/opt.html
%{_sysconfdir}/systemd/system/credentials-fetcher.service
%license LICENSE
%config /etc/opt/credentials-fetcher/config.json
%config /etc/opt/credentials-fetcher/env-file
# https://docs.fedoraproject.org/en-US/packaging-guidelines/LicensingGuidelines/
%doc CONTRIBUTING.md NOTICE README.md
# https://refspecs.linuxfoundation.org/FHS_3.0/fhs/ch03s13.html
%attr(0700, -, -) /opt/credentials-fetcher/lib/libcf_private.so
%attr(0700, -, -) /opt/credentials-fetcher/lib/libcf_gmsa_service_private.so
%attr(0700, -, -) /opt/credentials-fetcher/bin/credentials_fetcher_utf16_private.exe

%changelog
* Sun Aug 7 2022 Samiullah Mohammed <samiull@amazon.com> - 0.0.91
- Relocate binary, library files and change permissions
* Sat Jul 30 2022 Samiullah Mohammed <samiull@amazon.com> - 0.0.90
- add ctests and bump revision to 0.0.90
* Thu Jul 28 2022 Samiullah Mohammed <samiull@amazon.com> - 0.0.1
- Add mono-based utf16 decoder
* Tue Jul 12 2022 Samiullah Mohammed <samiull@amazon.com> - 0.0.1
- Resolve rpath for Fedora and change macros
* Sat Jun 18 2022 Sai Kiran Akula <saakla@amazon.com> - 0.0.1
- Refactor cmake for all the directories
* Thu Jun 16 2022 Samiullah Mohammed <samiull@amazon.com> - 0.0.1
- Compile subdirectory into a shared library
* Wed Jun 15 2022 Samiullah Mohammed <samiull@amazon.com> - 0.0.1
- Add daemon infra
* Wed Jun 8 2022 Samiullah Mohammed <samiull@amazon.com> - 0.0.1
- Fixes to rpm spec
* Mon Jun 6 2022 Samiullah Mohammed <samiull@amazon.com> - 0.0.1
- Initial commit
