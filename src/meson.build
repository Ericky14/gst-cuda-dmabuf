gst_dep = dependency('gstreamer-1.0')
gst_base_dep = dependency('gstreamer-base-1.0')
gst_video_dep = dependency('gstreamer-video-1.0')
gst_allocators_dep = dependency('gstreamer-allocators-1.0')
gbm_dep = dependency('gbm')
drm_dep = dependency('libdrm')

shared_library(
  'gstcudadmabuf',
  [
    'gbm_dmabuf_pool.c',
    'gstcudadmabufupload.c',
    'plugin.c',
  ],
  dependencies: [gst_dep, gst_base_dep, gst_video_dep, gst_allocators_dep, gbm_dep, drm_dep],
  install: true,
  install_dir: gst_dep.get_variable('pluginsdir')
)
