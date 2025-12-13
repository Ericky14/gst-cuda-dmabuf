gst_dep = dependency('gstreamer-1.0')
gst_video_dep = dependency('gstreamer-video-1.0')
gst_allocators_dep = dependency('gstreamer-allocators-1.0')

test_video_meta = executable(
  'test_video_meta',
  'test_video_meta.c',
  dependencies: [gst_dep, gst_video_dep, gst_allocators_dep],
  install: false
)

test('video_meta', test_video_meta)
