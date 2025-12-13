# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2025 Ericky

Name:           gst-cuda-dmabuf
Version:        0.1.0
Release:        1%{?dist}
Summary:        GStreamer CUDA DMA-BUF plugin for zero-copy video processing

License:        MIT
URL:            https://github.com/Ericky14/%{name}
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  meson >= 1.1.0
BuildRequires:  ninja-build
BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  pkgconfig(gstreamer-1.0) >= 1.24
BuildRequires:  pkgconfig(gstreamer-base-1.0)
BuildRequires:  pkgconfig(gstreamer-video-1.0)
BuildRequires:  pkgconfig(gbm)
BuildRequires:  pkgconfig(libdrm)
BuildRequires:  pkgconfig(egl)
BuildRequires:  cuda-devel >= 12.0

Requires:       gstreamer1 >= 1.24
Requires:       gstreamer1-plugins-base
Requires:       mesa-libgbm
Requires:       libdrm
Requires:       mesa-libEGL

%description
A GStreamer plugin that uploads video frames from CUDA device memory to 
Linux DMA-BUF, enabling zero-copy integration with Wayland compositors
and other DMA-BUF consumers. Converts NV12 to BGRX using CUDA kernels.

%prep
%autosetup -n %{name}-%{version}

%build
%meson
%meson_build

%install
%meson_install

%files
%license LICENSE
%doc README.md
%{_libdir}/gstreamer-1.0/libgstcudadmabuf.so

%changelog
* Fri Dec 13 2024 Ericky <ericky_k_y@hotmail.com> - 0.1.0-1
- Initial package
