#sbs-git:slp/unmodified/cairo cairo 1.11.3 076a40b95caaadbc4a05b92a1a1d7840427e05b7
Name:       cairo
Summary:    A vector graphics library
Version:    1.12.12
Release:    10
Group:      System/Libraries
License:    LGPLv2 or MPLv1.1
URL:        http://www.cairographics.org
Source0:    http://cairographics.org/releases/%{name}-%{version}.tar.gz
Source1001: packaging/cairo.manifest 

Requires(post): /sbin/ldconfig
Requires(postun): /sbin/ldconfig
BuildRequires:  pkgconfig(xrender)
BuildRequires:  pkgconfig(x11)
BuildRequires:  pkgconfig(xext)
BuildRequires:  pkgconfig(libpng)
BuildRequires:  pkgconfig(libxml-2.0)
BuildRequires:  pkgconfig(pixman-1)
BuildRequires:  pkgconfig(freetype2)
BuildRequires:  pkgconfig(fontconfig)
BuildRequires:  pkgconfig(glib-2.0)
BuildRequires:  pkgconfig(sm)
BuildRequires:  pkgconfig(xt)
BuildRequires:  pkgconfig(xcb)
BuildRequires:  pkgconfig(xcb-render)
BuildRequires:  pkgconfig(xcb-renderutil)
BuildRequires:  pkgconfig(xcb-shm)
BuildRequires:  pkgconfig(opengl-es-20)
#BuildRequires:  pkgconfig(librsvg-2.0)
BuildRequires:  binutils-devel
BuildRequires:  which
BuildRequires:  autoconf

%description
Cairo is a 2D graphics library with support for multiple output devices.

%package devel
Summary:    Development components for the cairo library
Group:      Development/Libraries
Requires:   %{name} = %{version}-%{release}
Requires:   pixman-devel

%description devel
cairo development libraries and head files

%prep
%setup -q -n %{name}-%{version}

%build
cp %{SOURCE1001} .
NOCONFIGURE=1 ./autogen.sh
%configure --disable-static \
    --disable-win32 \
    --enable-directfb=no \
    --enable-xlib \
    --with-x \
    --x-includes=%{_includedir} \
    --x-libraries=%{_libdir} \
    --disable-gtk-doc \
%ifarch %ix86
    --enable-xcb
%else
    --enable-xcb \
    --enable-egl=yes \
    --enable-glesv2=yes
%endif

make %{?jobs:-j%jobs}

%install
rm -rf %{buildroot}
%make_install
rm -rf $RPM_BUILD_ROOT/usr/share/gtk-doc
mkdir -p %{buildroot}/usr/share/license
cat COPYING COPYING-LGPL-2.1 COPYING-MPL-1.1 > %{buildroot}/usr/share/license/%{name}

%post -p /sbin/ldconfig

%postun -p /sbin/ldconfig

%files
%manifest cairo.manifest
%{_libdir}/libcairo.so.*
/usr/share/license/%{name}
%exclude %{_libdir}/libcairo-*.so.*

%files devel
%manifest cairo.manifest
%{_includedir}/*
%{_libdir}/libcairo*.so
%{_libdir}/libcairo-*.so.*
%{_libdir}/pkgconfig/*
%exclude %{_bindir}/cairo-trace
%exclude %{_libdir}/cairo/libcairo-trace.so
%exclude %{_libdir}/cairo/libcairo-trace.so.*

