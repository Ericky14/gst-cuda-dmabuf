# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2025 Ericky

# NVIDIA CUDA packages version varies by Fedora release:
# - Fedora 41: CUDA 12.9
# - Fedora 42: CUDA 13.1 (no 12.x available)
# - Fedora 43+: Not yet available
# See: https://developer.nvidia.com/cuda-downloads
%if 0%{?fedora} >= 43
%{error:NVIDIA CUDA packages are not yet available for Fedora %{fedora}.}
%endif

%if 0%{?fedora} >= 42
%global cuda_version 13-1
%else
%global cuda_version 12-9
%endif

Name:           gst-cuda-dmabuf
Version:        1.0.0
Release:        1%{?dist}
Summary:        GStreamer CUDA DMA-BUF plugin for zero-copy video processing

License:        MIT
URL:            https://github.com/Ericky14/%{name}
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  meson >= 1.1.0
BuildRequires:  ninja-build
BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  gstreamer1-plugins-bad-free-devel
BuildRequires:  pkgconfig(gstreamer-1.0) >= 1.24
BuildRequires:  pkgconfig(gstreamer-base-1.0)
BuildRequires:  pkgconfig(gstreamer-video-1.0)
BuildRequires:  pkgconfig(gstreamer-allocators-1.0)
BuildRequires:  pkgconfig(gbm)
BuildRequires:  pkgconfig(libdrm)
BuildRequires:  pkgconfig(egl)
BuildRequires:  cuda-cudart-devel-%{cuda_version}
BuildRequires:  cuda-driver-devel-%{cuda_version}
BuildRequires:  cuda-nvcc-%{cuda_version}

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
# Ensure /usr/local/cuda symlink points to the installed CUDA version
%if 0%{?fedora} >= 42
CUDA_DIR=/usr/local/cuda-13.1
%else
CUDA_DIR=/usr/local/cuda-12.9
%endif
if [ ! -L /usr/local/cuda ] && [ -d "$CUDA_DIR" ]; then
    ln -sf "$CUDA_DIR" /usr/local/cuda
fi
%meson
%meson_build

%install
%meson_install

%files
%license LICENSE
%doc README.md
%{_libdir}/gstreamer-1.0/libgstcudadmabuf.so

%changelog
* Sat Dec 13 2025 Ericky <ericky_k_y@hotmail.com> - 1.0.0-1
- Version bump to 1.0.0
* Fri Dec 13 2024 Ericky <ericky_k_y@hotmail.com> - 0.1.0-1
- Initial package
