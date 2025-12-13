/* SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2025 Ericky
 */

#define _GNU_SOURCE

#include "gbm_dmabuf_pool.h"

#include <gst/allocators/gstdmabuf.h>
#include <drm/drm_fourcc.h>
#include <gbm.h>

#include <fcntl.h>
#include <unistd.h>
#include <string.h>

G_DEFINE_TYPE(GstGbmDmaBufPool, gst_gbm_dmabuf_pool, GST_TYPE_BUFFER_POOL)

static gboolean
gst_gbm_dmabuf_pool_start(GstBufferPool *pool)
{
    GstGbmDmaBufPool *p = (GstGbmDmaBufPool *)pool;

    /* Use NVIDIA GPU (renderD129) as primary */
    p->drm_fd = open("/dev/dri/renderD129", O_RDWR | O_CLOEXEC);
    if (p->drm_fd < 0)
        return FALSE;

    p->gbm = gbm_create_device(p->drm_fd);
    if (!p->gbm)
        return FALSE;

    p->dmabuf_alloc = gst_dmabuf_allocator_new();
    return TRUE;
}

static gboolean
gst_gbm_dmabuf_pool_stop(GstBufferPool *pool)
{
    GstGbmDmaBufPool *p = (GstGbmDmaBufPool *)pool;

    if (p->dmabuf_alloc)
    {
        gst_object_unref(p->dmabuf_alloc);
        p->dmabuf_alloc = NULL;
    }
    if (p->gbm)
    {
        gbm_device_destroy(p->gbm);
        p->gbm = NULL;
    }
    if (p->drm_fd >= 0)
    {
        close(p->drm_fd);
        p->drm_fd = -1;
    }
    return TRUE;
}

/* Called by pool when it needs a new buffer */
static GstFlowReturn
gst_gbm_dmabuf_pool_alloc_buffer(GstBufferPool *pool,
                                 GstBuffer **buffer,
                                 GstBufferPoolAcquireParams *params)
{
    GstGbmDmaBufPool *p = (GstGbmDmaBufPool *)pool;
    (void)params;

    guint width = GST_VIDEO_INFO_WIDTH(&p->info);
    guint height = GST_VIDEO_INFO_HEIGHT(&p->info);

    struct gbm_bo *bo = NULL;

    /* Try to create with the requested modifier first (for zero-copy scanout) */
    if (p->modifier != DRM_FORMAT_MOD_INVALID && p->modifier != DRM_FORMAT_MOD_LINEAR)
    {
        uint64_t modifiers[] = {p->modifier};
        bo = gbm_bo_create_with_modifiers(
            p->gbm,
            width,
            height,
            p->gbm_format,
            modifiers,
            1);

        if (bo)
        {
            g_print("GBM: Created buffer with modifier 0x%016lx\n", p->modifier);
        }
        else
        {
            g_print("GBM: Failed to create with modifier 0x%016lx, falling back\n", p->modifier);
        }
    }

    /* Fallback to LINEAR if tiled creation failed or not requested */
    if (!bo)
    {
        bo = gbm_bo_create(
            p->gbm,
            width,
            height,
            p->gbm_format,
            GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);

        if (bo)
        {
            p->modifier = DRM_FORMAT_MOD_LINEAR;
            g_print("GBM: Created LINEAR buffer\n");
        }
    }

    if (!bo)
    {
        GST_ERROR_OBJECT(pool, "Failed to create GBM buffer object");
        return GST_FLOW_ERROR;
    }

    int fd = gbm_bo_get_fd(bo);
    if (fd < 0)
    {
        gbm_bo_destroy(bo);
        return GST_FLOW_ERROR;
    }

    guint stride = gbm_bo_get_stride(bo);
    gsize size = (gsize)stride * (gsize)height;

    g_print("GBM ALLOC: %ux%u, gbm_stride=%u, size=%zu\n", width, height, stride, size);

    GstMemory *mem = gst_dmabuf_allocator_alloc(p->dmabuf_alloc, fd, size);

    GstBuffer *buf = gst_buffer_new();
    gst_buffer_append_memory(buf, mem);

    /* Add video meta with actual BGRx format for proper stride/offset handling.
     * DMA_DRM is a caps-level concept; video meta needs the real pixel format. */
    GstVideoMeta *vmeta = gst_buffer_add_video_meta(
        buf,
        GST_VIDEO_FRAME_FLAG_NONE,
        GST_VIDEO_FORMAT_BGRx,
        width,
        height);

    vmeta->n_planes = 1;
    vmeta->stride[0] = stride;
    vmeta->offset[0] = 0;

    /* ensure GBM BO lifetime matches GstBuffer */
    GQuark q = g_quark_from_static_string("gbm-bo");
    gst_mini_object_set_qdata(
        GST_MINI_OBJECT(buf),
        q,
        bo,
        (GDestroyNotify)gbm_bo_destroy);

    *buffer = buf;
    return GST_FLOW_OK;
}

static const gchar *pool_options[] = {
    GST_BUFFER_POOL_OPTION_VIDEO_META,
    NULL};

static const gchar **
gst_gbm_dmabuf_pool_get_options(GstBufferPool *pool)
{
    (void)pool;
    return (const gchar **)pool_options;
}

static void
gst_gbm_dmabuf_pool_class_init(GstGbmDmaBufPoolClass *klass)
{
    GstBufferPoolClass *pool_class = GST_BUFFER_POOL_CLASS(klass);
    pool_class->start = gst_gbm_dmabuf_pool_start;
    pool_class->stop = gst_gbm_dmabuf_pool_stop;
    pool_class->alloc_buffer = gst_gbm_dmabuf_pool_alloc_buffer;
    pool_class->get_options = gst_gbm_dmabuf_pool_get_options;
}

static void
gst_gbm_dmabuf_pool_init(GstGbmDmaBufPool *p)
{
    p->drm_fd = -1;
    p->gbm = NULL;
    p->dmabuf_alloc = NULL;
    p->gbm_format = GBM_FORMAT_XRGB8888;
    p->modifier = DRM_FORMAT_MOD_INVALID;
}

GstBufferPool *
gst_gbm_dmabuf_pool_new(const GstVideoInfo *info, guint64 modifier)
{
    GstGbmDmaBufPool *p = g_object_new(GST_TYPE_GBM_DMABUF_POOL, NULL);
    p->info = *info;
    p->modifier = modifier;
    return GST_BUFFER_POOL(p);
}

guint64
gst_gbm_dmabuf_pool_get_modifier(GstGbmDmaBufPool *pool)
{
    return pool->modifier;
}
