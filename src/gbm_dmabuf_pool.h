#pragma once

#include <gst/gst.h>
#include <gst/video/video.h>

#include <gst/allocators/gstdmabuf.h>
#include <gbm.h>

G_BEGIN_DECLS

#define GST_TYPE_GBM_DMABUF_POOL (gst_gbm_dmabuf_pool_get_type())
G_DECLARE_FINAL_TYPE(GstGbmDmaBufPool, gst_gbm_dmabuf_pool, GST, GBM_DMABUF_POOL, GstBufferPool)

struct _GstGbmDmaBufPool
{
    GstBufferPool parent;
    GstVideoInfo info;
    int drm_fd;
    struct gbm_device *gbm;
    GstAllocator *dmabuf_alloc;
    guint32 gbm_format;
    guint64 modifier;     /* DRM modifier actually used */
};

GstBufferPool *gst_gbm_dmabuf_pool_new(const GstVideoInfo *info, guint64 modifier);
guint64 gst_gbm_dmabuf_pool_get_modifier(GstGbmDmaBufPool *pool);

G_END_DECLS
