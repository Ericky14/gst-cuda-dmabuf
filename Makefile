# GStreamer CUDA DMA-BUF Upload Plugin Makefile
# Convenience targets for building, testing, and profiling

BUILD_DIR = builddir
PLUGIN_PATH = $(BUILD_DIR)/src
TEST_VIDEO = $(HOME)/veil.mp4
VERSION = $(shell grep "version:" meson.build | head -1 | sed "s/.*version: '\([^']*\)'.*/\1/")
RPM_BUILD_DIR = $(CURDIR)/rpmbuild

# Export plugin path for all targets
export GST_PLUGIN_PATH := $(CURDIR)/$(PLUGIN_PATH):$(GST_PLUGIN_PATH)

.PHONY: all build clean rebuild install test test-fakesink test-waylandsink \
        profile profile-stats profile-gui inspect caps help \
        rpm rpm-prep rpm-build rpm-clean srpm

# =============================================================================
# Build targets
# =============================================================================

all: build

build:
	@if [ ! -d $(BUILD_DIR) ]; then \
		meson setup $(BUILD_DIR); \
	fi
	ninja -C $(BUILD_DIR)

clean:
	ninja -C $(BUILD_DIR) clean

rebuild: clean build

install:
	ninja -C $(BUILD_DIR) install

# =============================================================================
# RPM Package targets
# =============================================================================

rpm-prep:
	@echo "Creating RPM build directories..."
	@mkdir -p $(RPM_BUILD_DIR)/{BUILD,RPMS,SOURCES,SPECS,SRPMS}
	@echo "Creating source tarball..."
	@mkdir -p /tmp/gst-cuda-dmabuf-$(VERSION)
	@tar --exclude='.git' --exclude='builddir' --exclude='rpmbuild' --exclude='target' \
		--exclude='*.tar.gz' --exclude='*.rpm' --exclude='*.nsys-rep' \
		-czf $(RPM_BUILD_DIR)/SOURCES/gst-cuda-dmabuf-$(VERSION).tar.gz \
		--transform="s,^,gst-cuda-dmabuf-$(VERSION)/," \
		--exclude='gst-cuda-dmabuf-*.tar.gz' .
	@cp gst-cuda-dmabuf.spec $(RPM_BUILD_DIR)/SPECS/
	@echo "Source tarball created: $(RPM_BUILD_DIR)/SOURCES/gst-cuda-dmabuf-$(VERSION).tar.gz"

srpm: rpm-prep
	@echo "Building SRPM..."
	rpmbuild -bs --define "_topdir $(RPM_BUILD_DIR)" \
		$(RPM_BUILD_DIR)/SPECS/gst-cuda-dmabuf.spec
	@echo ""
	@echo "SRPM created:"
	@ls -lh $(RPM_BUILD_DIR)/SRPMS/*.src.rpm

rpm: rpm-prep
	@echo "Building RPM for version $(VERSION)..."
	rpmbuild -bb --define "_topdir $(RPM_BUILD_DIR)" \
		$(RPM_BUILD_DIR)/SPECS/gst-cuda-dmabuf.spec
	@echo ""
	@echo "RPMs created:"
	@ls -lh $(RPM_BUILD_DIR)/RPMS/*/*.rpm

rpm-clean:
	@echo "Cleaning RPM build directory..."
	rm -rf $(RPM_BUILD_DIR)

# =============================================================================
# Test targets
# =============================================================================

test: test-waylandsink

test-fakesink:
	@echo "Testing with fakesink (no display)..."
	gst-launch-1.0 filesrc location=$(TEST_VIDEO) ! qtdemux name=demux demux.video_0 ! \
		h264parse ! nvh264dec ! cudadmabufupload ! fakesink sync=false

test-waylandsink:
	@echo "Testing with waylandsink..."
	gst-launch-1.0 filesrc location=$(TEST_VIDEO) ! qtdemux name=demux demux.video_0 ! \
		h264parse ! nvh264dec ! cudadmabufupload ! waylandsink

test-debug:
	@echo "Testing with debug output..."
	GST_DEBUG=cudadmabufupload:5 gst-launch-1.0 filesrc location=$(TEST_VIDEO) ! \
		qtdemux name=demux demux.video_0 ! h264parse ! nvh264dec ! cudadmabufupload ! waylandsink

test-verbose:
	@echo "Testing with verbose GStreamer output..."
	GST_DEBUG=3 gst-launch-1.0 -v filesrc location=$(TEST_VIDEO) ! \
		qtdemux name=demux demux.video_0 ! h264parse ! nvh264dec ! cudadmabufupload ! waylandsink

# =============================================================================
# Profiling targets (NVIDIA Nsight Systems)
# =============================================================================

profile:
	@echo "Running nsys profile..."
	nsys profile -o report_$$(date +%Y%m%d_%H%M%S) \
		gst-launch-1.0 filesrc location=$(TEST_VIDEO) ! qtdemux name=demux demux.video_0 ! \
		h264parse ! nvh264dec ! cudadmabufupload ! fakesink sync=false
	@echo "Profile saved. Run 'make profile-stats' or 'make profile-gui' to view."

profile-stats:
	@echo "=== CUDA API Summary ==="
	@nsys stats $$(ls -t *.nsys-rep 2>/dev/null | head -1) 2>/dev/null || \
		echo "No .nsys-rep file found. Run 'make profile' first."

profile-cuda:
	@echo "=== CUDA GPU Kernel Summary ==="
	@nsys stats --report cuda_gpu_kern_sum $$(ls -t *.nsys-rep 2>/dev/null | head -1) 2>/dev/null || \
		echo "No .nsys-rep file found. Run 'make profile' first."

profile-memory:
	@echo "=== CUDA Memory Operations ==="
	@nsys stats --report cuda_gpu_mem_time_sum $$(ls -t *.nsys-rep 2>/dev/null | head -1) 2>/dev/null || \
		echo "No .nsys-rep file found. Run 'make profile' first."

profile-gui:
	@echo "Opening Nsight Systems GUI..."
	@nsys-ui $$(ls -t *.nsys-rep 2>/dev/null | head -1) 2>/dev/null || \
		echo "No .nsys-rep file found. Run 'make profile' first."

profile-clean:
	rm -f *.nsys-rep *.sqlite *.qdstrm

# =============================================================================
# GStreamer inspection targets
# =============================================================================

inspect:
	@echo "=== Plugin Info ==="
	gst-inspect-1.0 cudadmabufupload

caps:
	@echo "=== Negotiated Caps ==="
	GST_DEBUG=GST_CAPS:5 gst-launch-1.0 filesrc location=$(TEST_VIDEO) ! \
		qtdemux name=demux demux.video_0 ! h264parse ! nvh264dec ! cudadmabufupload ! fakesink 2>&1 | \
		grep -E "(caps|drm-format)" | head -50

negotiate:
	@echo "=== Full Pipeline Negotiation ==="
	gst-launch-1.0 -v filesrc location=$(TEST_VIDEO) ! qtdemux name=demux demux.video_0 ! \
		h264parse ! nvh264dec ! cudadmabufupload ! fakesink 2>&1 | grep -E "caps|negotiate"

# =============================================================================
# Benchmark targets
# =============================================================================

benchmark:
	@echo "=== Benchmark: 500 frames ==="
	@START=$$(date +%s.%N); \
	gst-launch-1.0 filesrc location=$(TEST_VIDEO) ! qtdemux name=demux demux.video_0 ! \
		h264parse ! nvh264dec ! cudadmabufupload ! fakesink sync=false num-buffers=500 2>/dev/null; \
	END=$$(date +%s.%N); \
	echo "Time: $$(echo "$$END - $$START" | bc) seconds"

fps:
	@echo "=== FPS Counter ==="
	gst-launch-1.0 filesrc location=$(TEST_VIDEO) ! qtdemux name=demux demux.video_0 ! \
		h264parse ! nvh264dec ! cudadmabufupload ! \
		fpsdisplaysink video-sink=fakesink sync=false text-overlay=false -v 2>&1 | \
		grep -E "rendered|dropped|fps"

# =============================================================================
# Development helpers
# =============================================================================

format:
	@echo "Formatting source files..."
	find src -name '*.c' -o -name '*.h' | xargs clang-format -i

check-leaks:
	@echo "Running with Valgrind (slow)..."
	@command -v valgrind >/dev/null 2>&1 || { echo "valgrind not found. Install with: sudo dnf install valgrind"; exit 1; }
	valgrind --leak-check=full --suppressions=nvidia.supp gst-launch-1.0 filesrc location=$(TEST_VIDEO) ! \
		qtdemux name=demux demux.video_0 ! h264parse ! nvh264dec ! cudadmabufupload ! fakesink num-buffers=100

check-leaks-full:
	@echo "Running with Valgrind (no suppressions)..."
	@command -v valgrind >/dev/null 2>&1 || { echo "valgrind not found. Install with: sudo dnf install valgrind"; exit 1; }
	valgrind --leak-check=full --show-reachable=yes gst-launch-1.0 filesrc location=$(TEST_VIDEO) ! \
		qtdemux name=demux demux.video_0 ! h264parse ! nvh264dec ! cudadmabufupload ! fakesink num-buffers=100

check-cuda-leaks:
	@echo "Running with compute-sanitizer..."
	@command -v compute-sanitizer >/dev/null 2>&1 || { echo "compute-sanitizer not found. Install CUDA Toolkit."; exit 1; }
	compute-sanitizer --tool memcheck gst-launch-1.0 filesrc location=$(TEST_VIDEO) ! \
		qtdemux name=demux demux.video_0 ! h264parse ! nvh264dec ! cudadmabufupload ! fakesink num-buffers=100

# =============================================================================
# Help
# =============================================================================

help:
	@echo "GStreamer CUDA DMA-BUF Upload Plugin"
	@echo ""
	@echo "Build targets:"
	@echo "  make build          - Build the plugin"
	@echo "  make clean          - Clean build artifacts"
	@echo "  make rebuild        - Clean and rebuild"
	@echo "  make install        - Install the plugin"
	@echo ""
	@echo "RPM Package targets:"
	@echo "  make rpm            - Build binary RPM"
	@echo "  make srpm           - Build source RPM"
	@echo "  make rpm-clean      - Clean RPM build directory"
	@echo ""
	@echo "Test targets:"
	@echo "  make test           - Test with waylandsink (default)"
	@echo "  make test-fakesink  - Test with fakesink (no display)"
	@echo "  make test-waylandsink - Test with Wayland compositor"
	@echo "  make test-debug     - Test with debug logging"
	@echo "  make test-verbose   - Test with verbose GStreamer output"
	@echo ""
	@echo "Profile targets (NVIDIA Nsight Systems):"
	@echo "  make profile        - Capture new profile"
	@echo "  make profile-stats  - Show summary of last profile"
	@echo "  make profile-cuda   - Show CUDA kernel summary"
	@echo "  make profile-memory - Show memory operation summary"
	@echo "  make profile-gui    - Open last profile in GUI"
	@echo "  make profile-clean  - Remove profile files"
	@echo ""
	@echo "Inspection targets:"
	@echo "  make inspect        - Show plugin info"
	@echo "  make caps           - Show negotiated caps"
	@echo "  make negotiate      - Show full negotiation"
	@echo ""
	@echo "Benchmark targets:"
	@echo "  make benchmark      - Time 500 frames"
	@echo "  make fps            - Show FPS counter"
	@echo ""
	@echo "Development:"
	@echo "  make format         - Format source with clang-format"
	@echo "  make check-leaks    - Run with Valgrind"
	@echo "  make check-cuda-leaks - Run with compute-sanitizer"
	@echo ""
	@echo "Variables:"
	@echo "  TEST_VIDEO=$(TEST_VIDEO)"
