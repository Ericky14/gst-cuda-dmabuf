/*
 * GStreamer CUDA DMA-BUF Upload Element
 *
 * Converts CUDA NV12 video to DMA-BUF for zero-copy compositor display.
 * Supports NV12 passthrough (preferred) and NV12→BGRx conversion paths.
 */

#include "gstcudadmabufupload.h"
#include "gbm_dmabuf_pool.h"
#include "cuda_egl_interop.h"
#include "pooled_buffers.h"
#include "drm_format_utils.h"
#include "caps_transform.h"
#include "buffer_transform.h"

#define GST_USE_UNSTABLE_API
#include <gst/video/video.h>
#include <gst/cuda/gstcuda.h>
#include <gst/allocators/allocators.h>
#include <wayland-client.h>
#include <drm/drm_fourcc.h>
#include <gbm.h>
#include <string.h>

/* Pool sizes for pre-allocated buffers */
#define NV12_POOL_SIZE 4
#define BGRX_POOL_SIZE 4

GST_DEBUG_CATEGORY(gst_cuda_dmabuf_upload_debug);
#define GST_CAT_DEFAULT gst_cuda_dmabuf_upload_debug

/* Private data structure */
struct _GstCudaDmabufUpload
{
    GstBaseTransform parent;

    /* Video info */
    GstVideoInfo info;
    GstVideoInfo cuda_info;

    /* Wayland display (if available) */
    struct wl_display *wl_display;

    /* Negotiated output format */
    guint64 negotiated_modifier;
    guint32 negotiated_fourcc;
    gboolean nv12_output;

    /* GStreamer pools */
    GstBufferPool *pool;
    GstBufferPool *cuda_pool;
    GstBufferPool *cuda_bgrx_pool;
    GstCudaContext *cuda_ctx;
    GstAllocator *dmabuf_allocator;

    /* Flags */
    gboolean cuda_input;

    /* CUDA-EGL interop context */
    CudaEglContext egl_ctx;

    /* Pre-allocated buffer pools */
    PooledBufferPool nv12_pool;
    PooledBufferPool bgrx_pool;

    /* Buffer transform context */
    BufferTransformContext btx;
};

G_DEFINE_TYPE(GstCudaDmabufUpload, gst_cuda_dmabuf_upload, GST_TYPE_BASE_TRANSFORM)

/* ============================================================================
 * Pad Templates
 * ============================================================================ */

/* Accept CUDA NV12 (preferred) or regular BGRx */
static GstStaticPadTemplate sink_template =
    GST_STATIC_PAD_TEMPLATE(
        "sink",
        GST_PAD_SINK,
        GST_PAD_ALWAYS,
        GST_STATIC_CAPS(
            "video/x-raw(memory:CUDAMemory),"
            "format=(string)NV12,"
            "width=(int)[1,MAX],"
            "height=(int)[1,MAX],"
            "framerate=(fraction)[0/1,MAX]"
            "; "
            "video/x-raw,"
            "format=(string)BGRx,"
            "width=(int)[1,MAX],"
            "height=(int)[1,MAX],"
            "framerate=(fraction)[0/1,MAX]"));

/* Output NV12 DMA-BUF (preferred) or XR24 DMA-BUF */
static GstStaticPadTemplate src_template =
    GST_STATIC_PAD_TEMPLATE(
        "src",
        GST_PAD_SRC,
        GST_PAD_ALWAYS,
        GST_STATIC_CAPS(
            /* NV12 with NVIDIA tiled modifiers - zero-copy passthrough */
            "video/x-raw(memory:DMABuf),"
            "format=(string)DMA_DRM,"
            "width=(int)[1,MAX],"
            "height=(int)[1,MAX],"
            "framerate=(fraction)[0/1,MAX],"
            "drm-format=(string){NV12:0x0300000000606010, NV12:0x0300000000606011, "
            "NV12:0x0300000000606012, NV12:0x0300000000606013, NV12:0x0300000000606014, "
            "NV12:0x0300000000606015, NV12:0x0300000000e08010, NV12:0x0300000000e08011, "
            "NV12:0x0300000000e08012, NV12:0x0300000000e08013, NV12:0x0300000000e08014, "
            "NV12:0x0300000000e08015, NV12:0x0, NV12:0x100000000000001}"
            "; "
            /* XR24 with NVIDIA tiled modifiers - fallback with conversion */
            "video/x-raw(memory:DMABuf),"
            "format=(string)DMA_DRM,"
            "width=(int)[1,MAX],"
            "height=(int)[1,MAX],"
            "framerate=(fraction)[0/1,MAX],"
            "drm-format=(string){XR24:0x0300000000606010, XR24:0x0300000000606011, "
            "XR24:0x0300000000606012, XR24:0x0300000000606013, XR24:0x0300000000606014, "
            "XR24:0x0300000000606015, XR24:0x0300000000e08010, XR24:0x0300000000e08011, "
            "XR24:0x0300000000e08012, XR24:0x0300000000e08013, XR24:0x0300000000e08014, "
            "XR24:0x0300000000e08015}"
            "; "
            "video/x-raw,"
            "format=(string)BGRx,"
            "width=(int)[1,MAX],"
            "height=(int)[1,MAX],"
            "framerate=(fraction)[0/1,MAX]"));

/* ============================================================================
 * Caps Handling
 * ============================================================================ */

static gboolean
gst_cuda_dmabuf_upload_set_caps(GstBaseTransform *base, GstCaps *incaps, GstCaps *outcaps)
{
    GstCudaDmabufUpload *self = GST_CUDA_DMABUF_UPLOAD(base);

    /* Check if input is CUDA memory */
    GstCapsFeatures *features = gst_caps_get_features(incaps, 0);
    self->cuda_input = gst_caps_features_contains(features, GST_CAPS_FEATURE_MEMORY_CUDA_MEMORY);

    GST_INFO_OBJECT(self, "Input is %s", self->cuda_input ? "CUDAMemory" : "regular memory");

    /* Parse output caps */
    GstStructure *s = gst_caps_get_structure(outcaps, 0);
    const gchar *format = gst_structure_get_string(s, "format");
    const gchar *drm_format = gst_structure_get_string(s, "drm-format");

    if (format && g_strcmp0(format, "DMA_DRM") == 0 && drm_format)
    {
        self->negotiated_modifier = drm_format_parse_modifier(drm_format);
        self->nv12_output = drm_format_is_nv12(drm_format);
        self->negotiated_fourcc = drm_format_get_fourcc(drm_format);

        GST_INFO_OBJECT(self, "Negotiated: %s (modifier: 0x%016lx, nv12=%d)",
                        drm_format, self->negotiated_modifier, self->nv12_output);
    }
    else
    {
        self->negotiated_modifier = DRM_FORMAT_MOD_INVALID;
        self->negotiated_fourcc = 0;
        self->nv12_output = FALSE;
    }

    /* Parse video info */
    if (!gst_video_info_from_caps(&self->info, incaps))
    {
        GST_ERROR_OBJECT(self, "Failed to parse video info");
        return FALSE;
    }

    if (self->cuda_input)
        self->cuda_info = self->info;

    return TRUE;
}

static GstCaps *
gst_cuda_dmabuf_upload_transform_caps(GstBaseTransform *base,
                                      GstPadDirection direction,
                                      GstCaps *caps,
                                      GstCaps *filter)
{
    GstCaps *outcaps;

    GST_DEBUG_OBJECT(base, "transform_caps direction=%s",
                     direction == GST_PAD_SINK ? "SINK" : "SRC");

    if (direction == GST_PAD_SINK)
    {
        /* sink → src */
        outcaps = caps_transform_sink_to_src(caps);
    }
    else
    {
        /* src → sink: reverse transform */
        if (gst_caps_get_size(caps) == 0 || gst_caps_is_any(caps))
        {
            outcaps = gst_static_pad_template_get_caps(&sink_template);
            goto filter_caps;
        }

        outcaps = caps_transform_src_to_sink(caps);

        if (gst_caps_is_empty(outcaps))
        {
            gst_caps_unref(outcaps);
            outcaps = gst_static_pad_template_get_caps(&sink_template);
        }
    }

filter_caps:
    if (filter)
    {
        GstCaps *tmp = gst_caps_intersect_full(outcaps, filter, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref(outcaps);
        outcaps = tmp;
    }

    return outcaps;
}

/* ============================================================================
 * Allocation
 * ============================================================================ */

static gboolean
gst_cuda_dmabuf_upload_propose_allocation(GstBaseTransform *base,
                                          GstQuery *decide_query,
                                          GstQuery *query)
{
    GstCudaDmabufUpload *self = GST_CUDA_DMABUF_UPLOAD(base);
    GstCaps *caps;
    GstVideoInfo info;

    gst_query_parse_allocation(query, &caps, NULL);
    if (!caps || !gst_video_info_from_caps(&info, caps))
        return FALSE;

    GstCapsFeatures *features = gst_caps_get_features(caps, 0);
    if (!gst_caps_features_contains(features, GST_CAPS_FEATURE_MEMORY_CUDA_MEMORY))
    {
        return GST_BASE_TRANSFORM_CLASS(gst_cuda_dmabuf_upload_parent_class)
            ->propose_allocation(base, decide_query, query);
    }

    /* Get CUDA context from upstream */
    GstQuery *ctx_query = gst_query_new_context("gst.cuda.context");
    if (gst_pad_peer_query(GST_BASE_TRANSFORM_SINK_PAD(base), ctx_query))
    {
        GstContext *ctx = NULL;
        gst_query_parse_context(ctx_query, &ctx);
        if (ctx)
        {
            const GstStructure *s = gst_context_get_structure(ctx);
            gst_structure_get(s, "gst.cuda.context", GST_TYPE_CUDA_CONTEXT, &self->cuda_ctx, NULL);
        }
    }
    gst_query_unref(ctx_query);

    if (!self->cuda_ctx)
    {
        GST_WARNING_OBJECT(self, "No CUDA context from upstream");
        return GST_BASE_TRANSFORM_CLASS(gst_cuda_dmabuf_upload_parent_class)
            ->propose_allocation(base, decide_query, query);
    }

    /* Create CUDA buffer pool with MMAP allocation */
    if (self->cuda_pool)
    {
        gst_buffer_pool_set_active(self->cuda_pool, FALSE);
        gst_object_unref(self->cuda_pool);
    }

    self->cuda_pool = gst_cuda_buffer_pool_new(self->cuda_ctx);
    GstStructure *config = gst_buffer_pool_get_config(self->cuda_pool);
    gst_buffer_pool_config_set_cuda_alloc_method(config, GST_CUDA_MEMORY_ALLOC_MMAP);

    guint size = GST_VIDEO_INFO_SIZE(&info);
    gst_buffer_pool_config_set_params(config, caps, size, 4, 0);
    gst_buffer_pool_config_add_option(config, GST_BUFFER_POOL_OPTION_VIDEO_META);

    if (!gst_buffer_pool_set_config(self->cuda_pool, config))
    {
        GST_ERROR_OBJECT(self, "Failed to configure CUDA pool");
        gst_object_unref(self->cuda_pool);
        self->cuda_pool = NULL;
        return FALSE;
    }

    gst_query_add_allocation_pool(query, self->cuda_pool, size, 4, 0);
    gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL);
    self->cuda_info = info;

    GST_INFO_OBJECT(self, "Proposed CUDA pool with MMAP allocation");
    return TRUE;
}

static gboolean
gst_cuda_dmabuf_upload_decide_allocation(GstBaseTransform *base, GstQuery *query)
{
    GstCudaDmabufUpload *self = GST_CUDA_DMABUF_UPLOAD(base);

    if (self->pool)
    {
        gst_buffer_pool_set_active(self->pool, FALSE);
        gst_object_unref(self->pool);
        self->pool = NULL;
    }

    if (self->negotiated_modifier == DRM_FORMAT_MOD_INVALID)
    {
        return GST_BASE_TRANSFORM_CLASS(gst_cuda_dmabuf_upload_parent_class)
            ->decide_allocation(base, query);
    }

    guint size = GST_VIDEO_INFO_SIZE(&self->info);
    self->pool = gst_gbm_dmabuf_pool_new(&self->info, self->negotiated_modifier);

    GstStructure *config = gst_buffer_pool_get_config(self->pool);
    GstCaps *caps = gst_pad_get_current_caps(GST_BASE_TRANSFORM_SRC_PAD(base));
    if (!caps)
    {
        caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "DMA_DRM", NULL);
        gst_caps_set_features(caps, 0, gst_caps_features_new("memory:DMABuf", NULL));
    }

    gst_buffer_pool_config_set_params(config, caps, size, 2, 4);
    gst_caps_unref(caps);
    gst_buffer_pool_config_add_option(config, GST_BUFFER_POOL_OPTION_VIDEO_META);

    if (!gst_buffer_pool_set_config(self->pool, config) ||
        !gst_buffer_pool_set_active(self->pool, TRUE))
    {
        GST_ERROR_OBJECT(self, "Failed to configure/activate pool");
        gst_object_unref(self->pool);
        self->pool = NULL;
        return FALSE;
    }

    gst_query_add_allocation_pool(query, self->pool, size, 2, 4);
    gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL);
    return TRUE;
}

/* ============================================================================
 * Transform
 * ============================================================================ */

static GstFlowReturn
gst_cuda_dmabuf_upload_prepare_output_buffer(GstBaseTransform *base,
                                             GstBuffer *inbuf,
                                             GstBuffer **outbuf)
{
    GstCudaDmabufUpload *self = GST_CUDA_DMABUF_UPLOAD(base);

    /* NV12 zero-copy passthrough path */
    if (self->cuda_input && self->nv12_output)
    {
        guint width = GST_VIDEO_INFO_WIDTH(&self->cuda_info);
        guint height = GST_VIDEO_INFO_HEIGHT(&self->cuda_info);

        /* Initialize buffer transform context if needed */
        if (!self->btx.egl_ctx)
        {
            if (!buffer_transform_context_init(&self->btx, &self->egl_ctx,
                                               self->negotiated_modifier))
            {
                GST_ERROR_OBJECT(self, "Failed to initialize buffer transform context");
                return GST_FLOW_ERROR;
            }
        }

        /* Initialize or reinitialize pool if needed */
        if (pooled_buffer_pool_needs_reinit(&self->nv12_pool, width, height))
        {
            pooled_buffer_pool_cleanup(&self->nv12_pool, &self->egl_ctx);
            if (!pooled_buffer_pool_init(&self->nv12_pool, &self->egl_ctx,
                                         NV12_POOL_SIZE, width, height,
                                         GBM_FORMAT_NV12, self->negotiated_modifier))
            {
                GST_ERROR_OBJECT(self, "Failed to initialize NV12 buffer pool");
                return GST_FLOW_ERROR;
            }
        }

        return buffer_transform_nv12_passthrough(&self->btx, &self->nv12_pool,
                                                 inbuf, outbuf, &self->cuda_info);
    }

    /* NV12→BGRx conversion path (CUDA input, XR24 output) */
    if (self->cuda_input)
    {
        /* Initialize buffer transform context if needed */
        if (!self->btx.egl_ctx)
        {
            if (!buffer_transform_context_init(&self->btx, &self->egl_ctx,
                                               self->negotiated_modifier))
            {
                GST_ERROR_OBJECT(self, "Failed to initialize buffer transform context");
                return GST_FLOW_ERROR;
            }
        }

        return buffer_transform_nv12_to_bgrx(&self->btx, inbuf, outbuf, &self->cuda_info);
    }

    /* Non-CUDA path: use GBM pool */
    if (!self->pool)
    {
        return GST_BASE_TRANSFORM_CLASS(gst_cuda_dmabuf_upload_parent_class)
            ->prepare_output_buffer(base, inbuf, outbuf);
    }

    return gst_buffer_pool_acquire_buffer(self->pool, outbuf, NULL);
}

static GstFlowReturn
gst_cuda_dmabuf_upload_transform(GstBaseTransform *base, GstBuffer *inbuf, GstBuffer *outbuf)
{
    GstCudaDmabufUpload *self = GST_CUDA_DMABUF_UPLOAD(base);

    /* CUDA paths handled in prepare_output_buffer */
    if (self->cuda_input)
        return GST_FLOW_OK;

    /* Non-CUDA: copy BGRx to DMABUF */
    return buffer_transform_bgrx_copy(inbuf, outbuf, &self->info);
}

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

static gboolean
gst_cuda_dmabuf_upload_start(GstBaseTransform *trans)
{
    GstCudaDmabufUpload *self = GST_CUDA_DMABUF_UPLOAD(trans);

    GstContext *ctx = gst_element_get_context(GST_ELEMENT(trans), "gst.wayland.display");
    if (ctx)
    {
        const GstStructure *s = gst_context_get_structure(ctx);
        gst_structure_get(s, "display", G_TYPE_POINTER, &self->wl_display, NULL);
        gst_context_unref(ctx);
    }

    return TRUE;
}

static void
gst_cuda_dmabuf_upload_finalize(GObject *object)
{
    GstCudaDmabufUpload *self = GST_CUDA_DMABUF_UPLOAD(object);

    /* Clean up buffer pools */
    pooled_buffer_pool_cleanup(&self->nv12_pool, &self->egl_ctx);
    pooled_buffer_pool_cleanup(&self->bgrx_pool, &self->egl_ctx);

    if (self->pool)
    {
        gst_buffer_pool_set_active(self->pool, FALSE);
        gst_object_unref(self->pool);
    }
    if (self->cuda_pool)
    {
        gst_buffer_pool_set_active(self->cuda_pool, FALSE);
        gst_object_unref(self->cuda_pool);
    }
    if (self->cuda_bgrx_pool)
    {
        gst_buffer_pool_set_active(self->cuda_bgrx_pool, FALSE);
        gst_object_unref(self->cuda_bgrx_pool);
    }
    if (self->cuda_ctx)
        gst_object_unref(self->cuda_ctx);
    if (self->dmabuf_allocator)
        gst_object_unref(self->dmabuf_allocator);
    if (self->btx.dmabuf_allocator)
        gst_object_unref(self->btx.dmabuf_allocator);

    /* Clean up CUDA-EGL context */
    cuda_egl_context_cleanup(&self->egl_ctx);

    G_OBJECT_CLASS(gst_cuda_dmabuf_upload_parent_class)->finalize(object);
}

static void
gst_cuda_dmabuf_upload_class_init(GstCudaDmabufUploadClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
    GstBaseTransformClass *base_class = GST_BASE_TRANSFORM_CLASS(klass);

    gobject_class->finalize = gst_cuda_dmabuf_upload_finalize;

    gst_element_class_add_pad_template(element_class,
                                       gst_static_pad_template_get(&sink_template));
    gst_element_class_add_pad_template(element_class,
                                       gst_static_pad_template_get(&src_template));

    gst_element_class_set_static_metadata(element_class,
                                          "CUDA → DMA-BUF Upload",
                                          "Filter/Video",
                                          "Zero-copy CUDA to DMA-BUF for Wayland compositor display",
                                          "Ericky");

    base_class->start = GST_DEBUG_FUNCPTR(gst_cuda_dmabuf_upload_start);
    base_class->set_caps = GST_DEBUG_FUNCPTR(gst_cuda_dmabuf_upload_set_caps);
    base_class->propose_allocation = GST_DEBUG_FUNCPTR(gst_cuda_dmabuf_upload_propose_allocation);
    base_class->decide_allocation = GST_DEBUG_FUNCPTR(gst_cuda_dmabuf_upload_decide_allocation);
    base_class->prepare_output_buffer = GST_DEBUG_FUNCPTR(gst_cuda_dmabuf_upload_prepare_output_buffer);
    base_class->transform = GST_DEBUG_FUNCPTR(gst_cuda_dmabuf_upload_transform);
    base_class->transform_caps = GST_DEBUG_FUNCPTR(gst_cuda_dmabuf_upload_transform_caps);
    base_class->passthrough_on_same_caps = FALSE;
}

static void
gst_cuda_dmabuf_upload_init(GstCudaDmabufUpload *self)
{
    gst_video_info_init(&self->info);
    gst_video_info_init(&self->cuda_info);
    self->negotiated_modifier = DRM_FORMAT_MOD_INVALID;
    memset(&self->egl_ctx, 0, sizeof(CudaEglContext));
    memset(&self->nv12_pool, 0, sizeof(PooledBufferPool));
    memset(&self->bgrx_pool, 0, sizeof(PooledBufferPool));
    memset(&self->btx, 0, sizeof(BufferTransformContext));
}
