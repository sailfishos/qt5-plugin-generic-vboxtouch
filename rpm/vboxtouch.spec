Name:		qt5-plugin-generic-vboxtouch
Version:	1.0
Release:	1
Summary:	Touchscreen driver for integrated mouse pointer in VirtualBox
Group:		Qt/Qt
License:	LGPLv2.1
URL:		http://github.com/nemomobile/qt5-plugin-generic-vboxtouch
Source0:	%{name}-%{version}.tar.bz2
Source1:        70-vboxtouch.rules
ExclusiveArch:  %{ix86} x86_64

BuildRequires:	pkgconfig(Qt5Core)
BuildRequires:	pkgconfig(Qt5Gui)
BuildRequires:	pkgconfig(Qt5Quick)

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
mkdir -p %{buildroot}/lib/udev/rules.d
cp %{S:1} %{buildroot}/lib/udev/rules.d/

%files
%defattr(-,root,root,-)
%{_libdir}/qt5/plugins/generic/libvboxtouchplugin.so
%{_libdir}/cmake/Qt5Gui/Qt5Gui_VirtualboxTouchScreenPlugin.cmake
/lib/udev/rules.d/*
