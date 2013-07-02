Name:		qpa-plugin-vboxtouch
Version:	1.0
Release:	1
Summary:	Touchscreen driver for integrated mouse pointer in VirtualBox
Group:		Qt/Qt
License:	LGPLv2.1
URL:		http://github.com/nemomobile/qpa-plugin-vboxtouch.git
Source0:	%{name}-%{version}.tar.bz2
ExclusiveArch:  %{ix86}

BuildRequires:	pkgconfig(Qt5Core)
BuildRequires:	pkgconfig(Qt5Gui)

%description
This driver extends Qt's platform support (QPA) for Virtualbox guests.
It uses the integrated pointer feature to create a smooth conversion from
the host pointer to touchscreen events in the guest, without grabbing the
host pointer.

%prep
%setup -q -n %{name}-%{version}/vboxtouch


%build
export QTDIR=/usr/share/qt5
%qmake5
make %{?jobs:-j%jobs}

%install
%qmake5_install

%files
%defattr(-,root,root,-)
%{_libdir}/qt5/plugins/generic/vboxtouch.so

