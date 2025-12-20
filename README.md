# GStreamer CUDA DMA-BUF Upload Plugin

A GStreamer element that converts CUDA NV12 video frames to DMA-BUF for zero-copy display on Wayland compositors (like COSMIC, Sway, etc.).

## Features

- **Zero-copy NV12 passthrough**: CUDA → DMA-BUF with NVIDIA tiled modifiers
- **NV12→BGRx GPU conversion**: Fallback path when compositor doesn't support NV12
- **Pre-allocated buffer pools**: Minimizes allocation overhead at runtime
- **Async CUDA operations**: Non-blocking plane copies with stream synchronization

## Requirements

### System Dependencies

- GStreamer 1.20+ with CUDA support (`gstreamer-cuda-1.0`)
- NVIDIA driver with DMA-BUF support (515+)
- CUDA Toolkit 12.x or 13.x
- GCC/G++ (GCC 14 required for CUDA 12.x with GCC 15)
- Mesa/GBM for DMA-BUF allocation
- EGL for CUDA-EGL interop

### Fedora/RHEL

```bash
sudo dnf install gstreamer1-devel gstreamer1-plugins-base-devel \
    gstreamer1-plugins-bad-free-devel mesa-libgbm-devel libdrm-devel \
    mesa-libEGL-devel meson ninja-build gcc-c++ \
    gstreamer1-plugin-nvidia  # For nvh264dec
```

### Ubuntu/Debian

```bash
sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    libgstreamer-plugins-bad1.0-dev libgbm-dev libdrm-dev \
    libegl1-mesa-dev meson ninja-build g++ \
    gstreamer1.0-plugins-bad  # For nvh264dec
```

## Building

### Quick Start

```bash
# Build
make build

# Test (requires a test video)
make test-fakesink
```

### Manual Build

```bash
meson setup builddir
ninja -C builddir
```

### Install System-Wide

```bash
sudo ninja -C builddir install
```

## Usage

### Basic Pipeline

```bash
# Decode H.264 with NVIDIA decoder → zero-copy to Wayland
gst-launch-1.0 filesrc location=video.mp4 ! qtdemux ! h264parse ! \
    nvh264dec ! cudadmabufupload ! waylandsink
```

### With Custom Test Video

```bash
export TEST_VIDEO=/path/to/your/video.mp4
make test-waylandsink
```

### Element Properties

The `cudadmabufupload` element has no configurable properties. It automatically:

1. Requests CUDA NV12 input from upstream (nvh264dec)
2. Negotiates NV12 DMA-BUF output with downstream (preferred)
3. Falls back to XR24 (BGRx) DMA-BUF if compositor doesn't support NV12

### Supported Formats

**Input:**
- `video/x-raw(memory:CUDAMemory), format=NV12` (preferred)
- `video/x-raw, format=BGRx`

**Output:**
- `video/x-raw(memory:DMABuf), format=DMA_DRM, drm-format=NV12:*` (zero-copy)
- `video/x-raw(memory:DMABuf), format=DMA_DRM, drm-format=XR24:*` (GPU conversion)

## Pipeline Architecture

```
┌─────────────┐    ┌──────────────┐    ┌───────────────────┐    ┌─────────────┐
│  filesrc    │───▶│  nvh264dec   │───▶│ cudadmabufupload  │───▶│ waylandsink │
│             │    │              │    │                   │    │             │
│  (file)     │    │ (CUDA NV12)  │    │   (DMA-BUF)       │    │ (display)   │
└─────────────┘    └──────────────┘    └───────────────────┘    └─────────────┘
                          │                      │
                          ▼                      ▼
                   CUDA Memory            GBM Buffer (DMA-BUF)
                   (GPU tiled)            (NVIDIA modifier)
```

## Makefile Targets

```bash
make help           # Show all available targets

# Build
make build          # Build the plugin
make rebuild        # Clean and rebuild
make install        # Install system-wide

# Packaging
make rpm            # Build RPM (Fedora/RHEL)
make deb            # Build .deb (Debian/Ubuntu/Pop!_OS)

# Test
make test-fakesink  # Test without display
make test-waylandsink  # Test with Wayland
make test-debug     # Test with debug logging

# Profile (requires NVIDIA Nsight Systems)
make profile        # Capture profile
make profile-stats  # Show summary
make profile-gui    # Open in GUI

# Development
make format         # Format source code
make check-leaks    # Run with Valgrind
make inspect        # Show plugin info
```

## Building Packages

### Fedora/RHEL (RPM)

```bash
make rpm
# Output: rpmbuild/RPMS/x86_64/gst-cuda-dmabuf-*.rpm
sudo dnf install rpmbuild/RPMS/x86_64/gst-cuda-dmabuf-*.rpm
```

### Debian/Ubuntu/Pop!_OS (DEB)

```bash
# Install build dependencies
sudo apt install debhelper-compat meson ninja-build pkg-config \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    libgstreamer-plugins-bad1.0-dev libgbm-dev libdrm-dev \
    libegl-dev nvidia-cuda-toolkit

# Build the package
make deb
# Output: ../gst-cuda-dmabuf_*.deb
sudo dpkg -i ../gst-cuda-dmabuf_*.deb
```

## Troubleshooting

### "No DMA-BUF modifiers supported"

Your compositor may not support NVIDIA's tiled modifiers. The element will
fall back to NV12→BGRx conversion (still GPU-accelerated).

### "Failed to initialize CUDA-EGL context"

Check that:
1. NVIDIA driver is loaded: `nvidia-smi`
2. DRM render node exists: `ls /dev/dri/renderD*`
3. EGL is working: `eglinfo`

### Valgrind Shows Leaks

NVIDIA drivers have known false positives. Use the included suppression file:

```bash
make check-leaks  # Uses nvidia.supp automatically
```

### GCC 15 Compatibility

If your system has GCC 15, CUDA 12.x requires GCC 14 instead (due to `type_traits` incompatibility). CUDA 13.x supports GCC 15.

For CUDA 12.x, install GCC 14:

```bash
# Fedora
sudo dnf install gcc-toolset-14-gcc-c++

# Ubuntu
sudo apt install g++-14
```

The build system automatically detects GCC 15 and uses GCC 14 for nvcc when available.

## License

MIT License - See [LICENSE](LICENSE) file.

## Contributing

Contributions are welcome! Please open issues or pull requests on GitHub.
