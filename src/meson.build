gst_dep = dependency('gstreamer-1.0')
gst_base_dep = dependency('gstreamer-base-1.0')
gst_video_dep = dependency('gstreamer-video-1.0')
gst_allocators_dep = dependency('gstreamer-allocators-1.0')
gst_cuda_dep = dependency('gstreamer-cuda-1.0')
gbm_dep = dependency('gbm')
drm_dep = dependency('libdrm')
egl_dep = dependency('egl')

# CUDA toolkit path
cuda_inc = include_directories('/usr/local/cuda/include')

# Find nvcc compiler (try common paths)
nvcc = find_program('nvcc', '/usr/local/cuda/bin/nvcc', '/usr/local/cuda-12.9/bin/nvcc', required: true)

# Compile CUDA kernel to object file
# Note: CUDA 12.9 requires GCC 14 (doesn't support GCC 15 type_traits yet)
# Use -D flags to prevent glibc 2.41 sinpi/cospi noexcept conflicts
cuda_kernel = custom_target(
  'cuda_nv12_to_bgrx',
  input: 'cuda_nv12_to_bgrx.cu',
  output: 'cuda_nv12_to_bgrx.o',
  command: [nvcc, '-c', '@INPUT@', '-o', '@OUTPUT@', 
            '-Xcompiler', '-fPIC',
            '-I/usr/local/cuda/include',
            '-D__GLIBC_USE_IEC_60559_FUNCS_EXT=0',
            '-D__GLIBC_USE_IEC_60559_FUNCS_EXT_C23=0',
            '-Wno-deprecated-gpu-targets'],
  # Note: Add --compiler-bindir=/usr/bin/gcc-14 locally if GCC 15 installed
  build_by_default: true
)

shared_library(
  'gstcudadmabuf',
  [
    'drm_format_utils.c',
    'cuda_egl_interop.c',
    'pooled_buffers.c',
    'caps_transform.c',
    'buffer_transform.c',
    'gbm_dmabuf_pool.c',
    'gstcudadmabufupload.c',
    'plugin.c',
  ],
  objects: cuda_kernel,
  dependencies: [gst_dep, gst_base_dep, gst_video_dep, gst_allocators_dep, gst_cuda_dep, gbm_dep, drm_dep, egl_dep],
  include_directories: cuda_inc,
  link_args: ['-L/usr/local/cuda/lib64', '-lcudart', '-lcuda'],
  install: true,
  install_dir: gst_dep.get_variable('pluginsdir')
)
