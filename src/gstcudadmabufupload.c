#include "gstcudadmabufupload.h"
#include "gbm_dmabuf_pool.h"
#include "cuda_nv12_to_bgrx.h"

#define GST_USE_UNSTABLE_API
#include <gst/video/video.h>
#include <gst/cuda/gstcuda.h>
#include <gst/allocators/allocators.h>
#include <wayland-client.h>
#include <drm/drm_fourcc.h>
#include <gbm.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h> /* for close() */
#include <fcntl.h>  /* for open() */
#include <cuda_runtime.h>
#include <cuda.h> /* CUDA driver API */

/* EGL and OpenGL for GBM → CUDA interop */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <cudaGL.h>  /* CUDA-GL interop */
#include <cudaEGL.h> /* CUDA-EGL interop */

struct _GstCudaDmabufUpload
{
    GstBaseTransform parent;
    GstVideoInfo info;
    GstVideoInfo cuda_info; /* Video info for CUDA input */
    struct wl_display *wl_display;
    guint64 negotiated_modifier;
    guint32 negotiated_fourcc;     /* DRM fourcc (e.g., DRM_FORMAT_NV12, DRM_FORMAT_XRGB8888) */
    gboolean nv12_output;          /* TRUE if outputting NV12 DMA-BUF (zero-copy passthrough) */
    GstBufferPool *pool;           /* Our GBM pool for output buffers */
    GstBufferPool *cuda_pool;      /* CUDA pool with exportable memory */
    GstBufferPool *cuda_bgrx_pool; /* CUDA pool for BGRx conversion output */
    GstCudaContext *cuda_ctx;
    gboolean cuda_input; /* TRUE if input is CUDAMemory */
    GstAllocator *dmabuf_allocator;

    /* GBM for tiled buffer allocation */
    int drm_fd;
    struct gbm_device *gbm;

    /* EGL for CUDA-GBM interop */
    EGLDisplay egl_display;
    EGLContext egl_context;
    gboolean egl_initialized;
};

G_DEFINE_TYPE(GstCudaDmabufUpload, gst_cuda_dmabuf_upload, GST_TYPE_BASE_TRANSFORM)

/* Parse drm-format string like "XR24:0x0300000000606010" to extract modifier */
static guint64
parse_drm_format_modifier(const gchar *drm_format)
{
    if (!drm_format)
        return DRM_FORMAT_MOD_INVALID;

    const gchar *colon = strchr(drm_format, ':');
    if (!colon)
        return DRM_FORMAT_MOD_INVALID;

    /* Skip the colon and parse the hex modifier */
    const gchar *mod_str = colon + 1;
    if (mod_str[0] == '0' && (mod_str[1] == 'x' || mod_str[1] == 'X'))
    {
        return strtoull(mod_str + 2, NULL, 16);
    }
    return strtoull(mod_str, NULL, 16);
}

/* Accept both CUDA memory (for true zero-copy) and regular BGRx */
static GstStaticPadTemplate sink_template =
    GST_STATIC_PAD_TEMPLATE(
        "sink",
        GST_PAD_SINK,
        GST_PAD_ALWAYS,
        GST_STATIC_CAPS(
            /* CUDA memory NV12 - GPU colorspace conversion path */
            "video/x-raw(memory:CUDAMemory),"
            "format=(string)NV12,"
            "width=(int)[1,MAX],"
            "height=(int)[1,MAX],"
            "framerate=(fraction)[0/1,MAX]"
            "; "
            /* Fallback: regular BGRx */
            "video/x-raw,"
            "format=(string)BGRx,"
            "width=(int)[1,MAX],"
            "height=(int)[1,MAX],"
            "framerate=(fraction)[0/1,MAX]"));

/* Advertise NV12 with NVIDIA tiled modifiers FIRST for zero-copy passthrough.
 * Also advertise XR24 (BGRx) with NVIDIA tiled modifiers as fallback.
 * We use GBM to allocate with these modifiers, then import into CUDA for GPU writes. */
static GstStaticPadTemplate src_template =
    GST_STATIC_PAD_TEMPLATE(
        "src",
        GST_PAD_SRC,
        GST_PAD_ALWAYS,
        GST_STATIC_CAPS(
            /* NV12 DMA_DRM with NVIDIA tiled modifiers - zero-copy YUV path (PREFERRED) */
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
            /* XR24 DMA_DRM with NVIDIA tiled modifiers - fallback with color conversion */
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
            /* Fallback: regular raw video */
            "video/x-raw,"
            "format=(string)BGRx,"
            "width=(int)[1,MAX],"
            "height=(int)[1,MAX],"
            "framerate=(fraction)[0/1,MAX]"));

static gboolean
gst_cuda_dmabuf_upload_set_caps(GstBaseTransform *base, GstCaps *incaps, GstCaps *outcaps)
{
    GstCudaDmabufUpload *self = GST_CUDA_DMABUF_UPLOAD(base);

    /* Check if input is CUDA memory */
    GstCapsFeatures *features = gst_caps_get_features(incaps, 0);
    self->cuda_input = gst_caps_features_contains(features, GST_CAPS_FEATURE_MEMORY_CUDA_MEMORY);

    if (self->cuda_input)
    {
        GST_INFO_OBJECT(self, "Input is CUDAMemory - using zero-copy path");
    }
    else
    {
        GST_INFO_OBJECT(self, "Input is regular memory - using copy path");
    }

    /* Parse output caps to get the negotiated drm-format */
    GstStructure *s = gst_caps_get_structure(outcaps, 0);
    const gchar *format = gst_structure_get_string(s, "format");
    const gchar *drm_format = gst_structure_get_string(s, "drm-format");

    /* Check if we're outputting DMA_DRM or regular video */
    if (format && g_strcmp0(format, "DMA_DRM") == 0 && drm_format)
    {
        self->negotiated_modifier = parse_drm_format_modifier(drm_format);

        /* Check if NV12 output (zero-copy passthrough) */
        self->nv12_output = g_str_has_prefix(drm_format, "NV12");
        if (self->nv12_output)
        {
            self->negotiated_fourcc = DRM_FORMAT_NV12;
            GST_INFO_OBJECT(self, "NV12 DMA-BUF output negotiated - zero-copy passthrough enabled!");
        }
        else
        {
            self->negotiated_fourcc = DRM_FORMAT_XRGB8888;
        }

        GST_INFO_OBJECT(self, "Negotiated drm-format: %s (modifier: 0x%016lx, nv12=%d)",
                        drm_format, self->negotiated_modifier, self->nv12_output);
    }
    else
    {
        /* Regular video format, no DMABUF */
        self->negotiated_modifier = DRM_FORMAT_MOD_INVALID;
        self->negotiated_fourcc = 0;
        self->nv12_output = FALSE;
        GST_INFO_OBJECT(self, "Negotiated regular video format: %s", format ? format : "unknown");
    }

    /* Parse video info from input caps */
    if (!gst_video_info_from_caps(&self->info, incaps))
    {
        GST_ERROR_OBJECT(self, "Failed to parse video info from input caps");
        return FALSE;
    }

    /* For CUDA input, also store as cuda_info */
    if (self->cuda_input)
    {
        self->cuda_info = self->info;
        GST_INFO_OBJECT(self, "CUDA input dimensions: %dx%d",
                        GST_VIDEO_INFO_WIDTH(&self->cuda_info),
                        GST_VIDEO_INFO_HEIGHT(&self->cuda_info));
    }

    return TRUE;
}

static GstCaps *
gst_cuda_dmabuf_upload_transform_caps(
    GstBaseTransform *base,
    GstPadDirection direction,
    GstCaps *caps,
    GstCaps *filter)
{
    GstCaps *outcaps;

    GST_DEBUG_OBJECT(base, "transform_caps direction=%s, caps=%" GST_PTR_FORMAT,
                     direction == GST_PAD_SINK ? "SINK" : "SRC", caps);

    if (direction == GST_PAD_SINK)
    {
        /* sink → src: Transform input caps to possible output caps */
        GstCapsFeatures *features = gst_caps_get_features(caps, 0);
        gboolean is_cuda = gst_caps_features_contains(features, GST_CAPS_FEATURE_MEMORY_CUDA_MEMORY);

        GstStructure *in_s = gst_caps_get_structure(caps, 0);
        const gchar *in_format = gst_structure_get_string(in_s, "format");

        /* Get dimensions from input */
        const GValue *w = gst_structure_get_value(in_s, "width");
        const GValue *h = gst_structure_get_value(in_s, "height");
        const GValue *fr = gst_structure_get_value(in_s, "framerate");

        if (is_cuda && g_strcmp0(in_format, "NV12") == 0)
        {
            /* CUDA NV12 input → prefer NV12 DMA_DRM for zero-copy,
             * fallback to XR24 DMA_DRM with GPU colorspace conversion */
            outcaps = gst_caps_new_empty();

            /* First: NV12 with NVIDIA tiled modifiers - compositor prefers these */
            static const char *nv12_nvidia_modifiers[] = {
                "NV12:0x0300000000606010",
                "NV12:0x0300000000606011",
                "NV12:0x0300000000606012",
                "NV12:0x0300000000606013",
                "NV12:0x0300000000606014",
                "NV12:0x0300000000606015",
                "NV12:0x0300000000e08010",
                "NV12:0x0300000000e08011",
                "NV12:0x0300000000e08012",
                "NV12:0x0300000000e08013",
                "NV12:0x0300000000e08014",
                "NV12:0x0300000000e08015",
                "NV12:0x0",               /* LINEAR fallback */
                "NV12:0x100000000000001", /* INVALID/AUTO (let GBM choose) */
                NULL};

            for (int i = 0; nv12_nvidia_modifiers[i]; i++)
            {
                GstCaps *tmp = gst_caps_new_simple(
                    "video/x-raw",
                    "format", G_TYPE_STRING, "DMA_DRM",
                    "drm-format", G_TYPE_STRING, nv12_nvidia_modifiers[i],
                    NULL);
                gst_caps_set_features(tmp, 0,
                                      gst_caps_features_new("memory:DMABuf", NULL));

                GstStructure *out_s = gst_caps_get_structure(tmp, 0);
                if (w)
                    gst_structure_set_value(out_s, "width", w);
                if (h)
                    gst_structure_set_value(out_s, "height", h);
                if (fr)
                    gst_structure_set_value(out_s, "framerate", fr);

                gst_caps_append(outcaps, tmp);
            }

            /* Second: XR24 with NVIDIA tiled modifiers - needs GPU conversion */
            static const char *nvidia_modifiers[] = {
                "XR24:0x0300000000606010",
                "XR24:0x0300000000606011",
                "XR24:0x0300000000606012",
                "XR24:0x0300000000606013",
                "XR24:0x0300000000606014",
                "XR24:0x0300000000606015",
                NULL};

            for (int i = 0; nvidia_modifiers[i]; i++)
            {
                GstCaps *tmp = gst_caps_new_simple(
                    "video/x-raw",
                    "format", G_TYPE_STRING, "DMA_DRM",
                    "drm-format", G_TYPE_STRING, nvidia_modifiers[i],
                    NULL);
                gst_caps_set_features(tmp, 0,
                                      gst_caps_features_new("memory:DMABuf", NULL));

                GstStructure *out_s = gst_caps_get_structure(tmp, 0);
                if (w)
                    gst_structure_set_value(out_s, "width", w);
                if (h)
                    gst_structure_set_value(out_s, "height", h);
                if (fr)
                    gst_structure_set_value(out_s, "framerate", fr);

                gst_caps_append(outcaps, tmp);
            }
        }
        else if (g_strcmp0(in_format, "BGRx") == 0)
        {
            /* BGRx input → output XR24 DMA_DRM with NVIDIA tiled modifiers */
            outcaps = gst_caps_new_empty();

            static const char *nvidia_modifiers[] = {
                "XR24:0x0300000000606010",
                "XR24:0x0300000000606011",
                "XR24:0x0300000000606012",
                NULL};

            for (int i = 0; nvidia_modifiers[i]; i++)
            {
                GstCaps *tmp = gst_caps_new_simple(
                    "video/x-raw",
                    "format", G_TYPE_STRING, "DMA_DRM",
                    "drm-format", G_TYPE_STRING, nvidia_modifiers[i],
                    NULL);
                gst_caps_set_features(tmp, 0,
                                      gst_caps_features_new("memory:DMABuf", NULL));

                GstStructure *out_s = gst_caps_get_structure(tmp, 0);
                if (w)
                    gst_structure_set_value(out_s, "width", w);
                if (h)
                    gst_structure_set_value(out_s, "height", h);
                if (fr)
                    gst_structure_set_value(out_s, "framerate", fr);

                gst_caps_append(outcaps, tmp);
            }
        }
        else
        {
            /* Unknown input format - return empty */
            outcaps = gst_caps_new_empty();
        }
    }
    else
    {
        /* src → sink: Given downstream wants these output caps, what input do we need? */

        /* Handle empty or ANY caps - return our full sink template */
        if (gst_caps_get_size(caps) == 0 || gst_caps_is_any(caps))
        {
            outcaps = gst_static_pad_template_get_caps(&sink_template);
            goto filter_caps;
        }

        outcaps = gst_caps_new_empty();

        /* Check each structure in the caps */
        for (guint i = 0; i < gst_caps_get_size(caps); i++)
        {
            GstStructure *out_s = gst_caps_get_structure(caps, i);
            GstCapsFeatures *features = gst_caps_get_features(caps, i);
            const gchar *format = gst_structure_get_string(out_s, "format");

            /* Get dimensions from output caps */
            const GValue *w = gst_structure_get_value(out_s, "width");
            const GValue *h = gst_structure_get_value(out_s, "height");
            const GValue *fr = gst_structure_get_value(out_s, "framerate");

            /* Check if this is a DMABuf with DMA_DRM format */
            gboolean is_dmabuf = features && gst_caps_features_contains(features, "memory:DMABuf");

            if (format && g_strcmp0(format, "DMA_DRM") == 0 && is_dmabuf)
            {
                /* Get drm-format - could be a string or a list */
                const GValue *drm_val = gst_structure_get_value(out_s, "drm-format");
                gboolean has_nv12 = FALSE;
                gboolean has_xr24 = FALSE;

                if (drm_val)
                {
                    if (G_VALUE_HOLDS_STRING(drm_val))
                    {
                        const gchar *drm_format = g_value_get_string(drm_val);
                        has_nv12 = g_str_has_prefix(drm_format, "NV12");
                        has_xr24 = g_str_has_prefix(drm_format, "XR24");
                    }
                    else if (GST_VALUE_HOLDS_LIST(drm_val))
                    {
                        guint n = gst_value_list_get_size(drm_val);
                        for (guint j = 0; j < n; j++)
                        {
                            const GValue *item = gst_value_list_get_value(drm_val, j);
                            if (G_VALUE_HOLDS_STRING(item))
                            {
                                const gchar *drm_format = g_value_get_string(item);
                                if (g_str_has_prefix(drm_format, "NV12"))
                                    has_nv12 = TRUE;
                                if (g_str_has_prefix(drm_format, "XR24"))
                                    has_xr24 = TRUE;
                            }
                        }
                    }
                }

                if (has_nv12)
                {
                    /* NV12 DMABUF output → need CUDA NV12 input */
                    GstCaps *tmp = gst_caps_new_simple(
                        "video/x-raw",
                        "format", G_TYPE_STRING, "NV12",
                        NULL);
                    gst_caps_set_features(tmp, 0,
                                          gst_caps_features_new(GST_CAPS_FEATURE_MEMORY_CUDA_MEMORY, NULL));

                    GstStructure *s = gst_caps_get_structure(tmp, 0);
                    if (w)
                        gst_structure_set_value(s, "width", w);
                    if (h)
                        gst_structure_set_value(s, "height", h);
                    if (fr)
                        gst_structure_set_value(s, "framerate", fr);

                    gst_caps_append(outcaps, tmp);
                }

                if (has_xr24)
                {
                    /* XR24 DMABUF output → need CUDA NV12 input (we do GPU conversion) */
                    GstCaps *tmp = gst_caps_new_simple(
                        "video/x-raw",
                        "format", G_TYPE_STRING, "NV12",
                        NULL);
                    gst_caps_set_features(tmp, 0,
                                          gst_caps_features_new(GST_CAPS_FEATURE_MEMORY_CUDA_MEMORY, NULL));

                    GstStructure *s = gst_caps_get_structure(tmp, 0);
                    if (w)
                        gst_structure_set_value(s, "width", w);
                    if (h)
                        gst_structure_set_value(s, "height", h);
                    if (fr)
                        gst_structure_set_value(s, "framerate", fr);

                    gst_caps_append(outcaps, tmp);

                    /* Also accept regular BGRx as fallback */
                    tmp = gst_caps_new_simple(
                        "video/x-raw",
                        "format", G_TYPE_STRING, "BGRx",
                        NULL);

                    s = gst_caps_get_structure(tmp, 0);
                    if (w)
                        gst_structure_set_value(s, "width", w);
                    if (h)
                        gst_structure_set_value(s, "height", h);
                    if (fr)
                        gst_structure_set_value(s, "framerate", fr);

                    gst_caps_append(outcaps, tmp);
                }
            }
            else if (format && g_strcmp0(format, "BGRx") == 0)
            {
                /* Plain BGRx output → need BGRx input (passthrough) */
                GstCaps *tmp = gst_caps_new_simple(
                    "video/x-raw",
                    "format", G_TYPE_STRING, "BGRx",
                    NULL);

                GstStructure *s = gst_caps_get_structure(tmp, 0);
                if (w)
                    gst_structure_set_value(s, "width", w);
                if (h)
                    gst_structure_set_value(s, "height", h);
                if (fr)
                    gst_structure_set_value(s, "framerate", fr);

                gst_caps_append(outcaps, tmp);
            }
        }

        /* If we couldn't match any caps, return our template as fallback */
        if (gst_caps_is_empty(outcaps))
        {
            gst_caps_unref(outcaps);
            outcaps = gst_static_pad_template_get_caps(&sink_template);
        }
    }

filter_caps:
    GST_DEBUG_OBJECT(base, "transform_caps result before filter: %" GST_PTR_FORMAT, outcaps);

    if (filter)
    {
        GstCaps *tmp = gst_caps_intersect_full(
            outcaps, filter, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref(outcaps);
        outcaps = tmp;
    }

    GST_DEBUG_OBJECT(base, "transform_caps final result: %" GST_PTR_FORMAT, outcaps);
    return outcaps;
}

/* Propose allocation to upstream - provide CUDA pool with exportable memory */
static gboolean
gst_cuda_dmabuf_upload_propose_allocation(GstBaseTransform *base,
                                          GstQuery *decide_query,
                                          GstQuery *query)
{
    GstCudaDmabufUpload *self = GST_CUDA_DMABUF_UPLOAD(base);
    GstCaps *caps;
    GstVideoInfo info;

    gst_query_parse_allocation(query, &caps, NULL);
    if (!caps)
        return FALSE;

    if (!gst_video_info_from_caps(&info, caps))
        return FALSE;

    /* Check if upstream wants to provide CUDAMemory */
    GstCapsFeatures *features = gst_caps_get_features(caps, 0);
    if (!gst_caps_features_contains(features, GST_CAPS_FEATURE_MEMORY_CUDA_MEMORY))
    {
        /* Not CUDA memory, use default handling */
        return GST_BASE_TRANSFORM_CLASS(gst_cuda_dmabuf_upload_parent_class)
            ->propose_allocation(base, decide_query, query);
    }

    /* Get CUDA context from upstream element via context query */
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
        GST_WARNING_OBJECT(self, "No CUDA context available from upstream");
        return GST_BASE_TRANSFORM_CLASS(gst_cuda_dmabuf_upload_parent_class)
            ->propose_allocation(base, decide_query, query);
    }

    /* Create CUDA buffer pool with virtual memory allocation (exportable) */
    if (self->cuda_pool)
    {
        gst_buffer_pool_set_active(self->cuda_pool, FALSE);
        gst_object_unref(self->cuda_pool);
    }

    self->cuda_pool = gst_cuda_buffer_pool_new(self->cuda_ctx);

    GstStructure *config = gst_buffer_pool_get_config(self->cuda_pool);

    /* Set allocation method to MMAP (virtual memory - exportable as DMABUF) */
    gst_buffer_pool_config_set_cuda_alloc_method(config, GST_CUDA_MEMORY_ALLOC_MMAP);

    guint size = GST_VIDEO_INFO_SIZE(&info);
    gst_buffer_pool_config_set_params(config, caps, size, 4, 0);
    gst_buffer_pool_config_add_option(config, GST_BUFFER_POOL_OPTION_VIDEO_META);

    if (!gst_buffer_pool_set_config(self->cuda_pool, config))
    {
        GST_ERROR_OBJECT(self, "Failed to configure CUDA pool with MMAP allocation");
        gst_object_unref(self->cuda_pool);
        self->cuda_pool = NULL;
        return FALSE;
    }

    gst_query_add_allocation_pool(query, self->cuda_pool, size, 4, 0);
    gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL);

    self->cuda_info = info;

    GST_INFO_OBJECT(self, "Proposed CUDA pool with MMAP allocation (exportable) to upstream");

    return TRUE;
}

static gboolean
gst_cuda_dmabuf_upload_decide_allocation(GstBaseTransform *base, GstQuery *query)
{
    GstCudaDmabufUpload *self = GST_CUDA_DMABUF_UPLOAD(base);

    /* Release old pool if any */
    if (self->pool)
    {
        gst_buffer_pool_set_active(self->pool, FALSE);
        gst_object_unref(self->pool);
        self->pool = NULL;
    }

    /* If not using DMABUF, let GstBaseTransform handle allocation */
    if (self->negotiated_modifier == DRM_FORMAT_MOD_INVALID)
    {
        GST_DEBUG_OBJECT(self, "Using regular video output, not creating GBM pool");
        return GST_BASE_TRANSFORM_CLASS(gst_cuda_dmabuf_upload_parent_class)
            ->decide_allocation(base, query);
    }

    guint size = GST_VIDEO_INFO_SIZE(&self->info);

    /* Create pool with the negotiated modifier */
    self->pool = gst_gbm_dmabuf_pool_new(&self->info, self->negotiated_modifier);

    GstStructure *config = gst_buffer_pool_get_config(self->pool);

    /* Get the negotiated caps from the src pad */
    GstCaps *caps = gst_pad_get_current_caps(GST_BASE_TRANSFORM_SRC_PAD(base));
    if (!caps)
    {
        /* Fallback if no caps are set yet */
        caps = gst_caps_new_simple(
            "video/x-raw",
            "format", G_TYPE_STRING, "DMA_DRM",
            NULL);
        GstCapsFeatures *features =
            gst_caps_features_new("memory:DMABuf", NULL);
        gst_caps_set_features(caps, 0, features);
    }

    gst_buffer_pool_config_set_params(
        config,
        caps,
        size,
        2,
        4);

    gst_caps_unref(caps);

    gst_buffer_pool_config_add_option(config, GST_BUFFER_POOL_OPTION_VIDEO_META);

    if (!gst_buffer_pool_set_config(self->pool, config))
    {
        gst_object_unref(self->pool);
        self->pool = NULL;
        return FALSE;
    }

    if (!gst_buffer_pool_set_active(self->pool, TRUE))
    {
        GST_ERROR_OBJECT(self, "Failed to activate buffer pool");
        gst_object_unref(self->pool);
        self->pool = NULL;
        return FALSE;
    }

    GST_DEBUG_OBJECT(self, "GBM pool created and activated successfully");

    gst_query_add_allocation_pool(query, self->pool, size, 2, 4);
    gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL);
    return TRUE;
}

static CUresult
copy_plane_to_eglframe(const void *src_dev, size_t src_pitch,
                       CUeglFrame *dst, int plane,
                       size_t width_bytes, size_t height_rows)
{
    CUDA_MEMCPY2D c;
    memset(&c, 0, sizeof(c));
    c.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    c.srcDevice = (CUdeviceptr)src_dev;
    c.srcPitch = src_pitch;

    if (dst->frameType == CU_EGL_FRAME_TYPE_PITCH)
    {
        c.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        c.dstDevice = (CUdeviceptr)dst->frame.pPitch[plane];
        c.dstPitch = dst->pitch; /* NOTE: some drivers use one pitch for both planes */
        c.WidthInBytes = width_bytes;
        c.Height = height_rows;
        return cuMemcpy2D(&c);
    }
    else if (dst->frameType == CU_EGL_FRAME_TYPE_ARRAY)
    {
        c.dstMemoryType = CU_MEMORYTYPE_ARRAY;
        c.dstArray = dst->frame.pArray[plane];
        c.WidthInBytes = width_bytes;
        c.Height = height_rows;
        return cuMemcpy2D(&c);
    }

    return CUDA_ERROR_INVALID_VALUE;
}

static GstFlowReturn
gst_cuda_dmabuf_upload_prepare_output_buffer(GstBaseTransform *base,
                                             GstBuffer *inbuf,
                                             GstBuffer **outbuf)
{
    GstCudaDmabufUpload *self = GST_CUDA_DMABUF_UPLOAD(base);

    /* For CUDA NV12 input with NV12 output: zero-copy passthrough
     * Copy NV12 planes from CUDA to GBM buffer (no colorspace conversion needed)
     */
    if (self->cuda_input && self->nv12_output)
    {
        GstMemory *mem = gst_buffer_peek_memory(inbuf, 0);
        if (!gst_is_cuda_memory(mem))
        {
            GST_ERROR_OBJECT(self, "Expected CUDA memory");
            return GST_FLOW_ERROR;
        }

        guint width = GST_VIDEO_INFO_WIDTH(&self->cuda_info);
        guint height = GST_VIDEO_INFO_HEIGHT(&self->cuda_info);

        /* Read strides/offsets from GstVideoMeta (don’t guess) */
        GstVideoMeta *in_vmeta = gst_buffer_get_video_meta(inbuf);
        gint y_stride_in, uv_stride_in;
        gsize uv_offset_in;

        if (in_vmeta && in_vmeta->n_planes >= 2)
        {
            y_stride_in = in_vmeta->stride[0];
            uv_stride_in = in_vmeta->stride[1];
            uv_offset_in = in_vmeta->offset[1];
        }
        else
        {
            y_stride_in = width;
            uv_stride_in = width;
            uv_offset_in = (gsize)width * height;
        }

        /* Init GBM */
        if (!self->gbm)
        {
            self->drm_fd = open("/dev/dri/renderD129", O_RDWR | O_CLOEXEC);
            if (self->drm_fd < 0)
            {
                GST_ERROR_OBJECT(self, "open(renderD129) failed");
                return GST_FLOW_ERROR;
            }
            self->gbm = gbm_create_device(self->drm_fd);
            if (!self->gbm)
            {
                GST_ERROR_OBJECT(self, "gbm_create_device failed");
                close(self->drm_fd);
                self->drm_fd = -1;
                return GST_FLOW_ERROR;
            }
        }

        /* Allocate NV12 GBM BO (try modifier first, else linear) */
        struct gbm_bo *bo = NULL;
        if (self->negotiated_modifier != DRM_FORMAT_MOD_INVALID &&
            self->negotiated_modifier != DRM_FORMAT_MOD_LINEAR)
        {
            uint64_t mods[] = {self->negotiated_modifier};
            bo = gbm_bo_create_with_modifiers(self->gbm, width, height, GBM_FORMAT_NV12, mods, 1);
        }
        if (!bo)
        {
            bo = gbm_bo_create(self->gbm, width, height, GBM_FORMAT_NV12,
                               GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
        }
        if (!bo)
        {
            GST_ERROR_OBJECT(self, "Failed to allocate NV12 GBM BO");
            return GST_FLOW_ERROR;
        }

        int dmabuf_fd = gbm_bo_get_fd(bo);
        if (dmabuf_fd < 0)
        {
            GST_ERROR_OBJECT(self, "gbm_bo_get_fd failed");
            gbm_bo_destroy(bo);
            return GST_FLOW_ERROR;
        }

        guint y_stride_out = gbm_bo_get_stride_for_plane(bo, 0);
        guint uv_stride_out = gbm_bo_get_stride_for_plane(bo, 1);
        guint uv_offset_out = gbm_bo_get_offset(bo, 1);

        /* Init EGL once */
        if (!self->egl_initialized)
        {
            PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT =
                (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");

            if (eglGetPlatformDisplayEXT)
                self->egl_display = eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_MESA, self->gbm, NULL);
            else
                self->egl_display = eglGetDisplay((EGLNativeDisplayType)self->gbm);

            if (self->egl_display == EGL_NO_DISPLAY)
            {
                GST_ERROR_OBJECT(self, "eglGetDisplay failed: 0x%x", eglGetError());
                close(dmabuf_fd);
                gbm_bo_destroy(bo);
                return GST_FLOW_ERROR;
            }

            EGLint major, minor;
            if (!eglInitialize(self->egl_display, &major, &minor))
            {
                GST_ERROR_OBJECT(self, "eglInitialize failed: 0x%x", eglGetError());
                self->egl_display = EGL_NO_DISPLAY;
                close(dmabuf_fd);
                gbm_bo_destroy(bo);
                return GST_FLOW_ERROR;
            }

            self->egl_initialized = TRUE;

            CUresult cu_res = cuInit(0);
            if (cu_res != CUDA_SUCCESS)
            {
                GST_ERROR_OBJECT(self, "cuInit failed: %d", cu_res);
                close(dmabuf_fd);
                gbm_bo_destroy(bo);
                return GST_FLOW_ERROR;
            }
        }

        PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR =
            (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
        PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR =
            (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");

        if (!eglCreateImageKHR || !eglDestroyImageKHR)
        {
            GST_ERROR_OBJECT(self, "EGL_KHR_image missing");
            close(dmabuf_fd);
            gbm_bo_destroy(bo);
            return GST_FLOW_ERROR;
        }

        uint64_t mod = gbm_bo_get_modifier(bo);

        /* IMPORTANT: use DRM_FORMAT_NV12 for EGL_LINUX_DRM_FOURCC_EXT */
        EGLint attribs[] = {
            EGL_WIDTH, (EGLint)width,
            EGL_HEIGHT, (EGLint)height,
            EGL_LINUX_DRM_FOURCC_EXT, (EGLint)DRM_FORMAT_NV12,

            EGL_DMA_BUF_PLANE0_FD_EXT, dmabuf_fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)y_stride_out,
            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(mod & 0xffffffff),
            EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(mod >> 32),

            EGL_DMA_BUF_PLANE1_FD_EXT, dmabuf_fd,
            EGL_DMA_BUF_PLANE1_OFFSET_EXT, (EGLint)uv_offset_out,
            EGL_DMA_BUF_PLANE1_PITCH_EXT, (EGLint)uv_stride_out,
            EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, (EGLint)(mod & 0xffffffff),
            EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT, (EGLint)(mod >> 32),

            EGL_NONE};

        EGLImageKHR egl_image = eglCreateImageKHR(self->egl_display, EGL_NO_CONTEXT,
                                                  EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
        if (egl_image == EGL_NO_IMAGE_KHR)
        {
            GST_ERROR_OBJECT(self, "eglCreateImageKHR(NV12) failed: 0x%x", eglGetError());
            close(dmabuf_fd);
            gbm_bo_destroy(bo);
            return GST_FLOW_ERROR;
        }

        CUgraphicsResource res = NULL;
        CUresult cu_res = cuGraphicsEGLRegisterImage(&res, egl_image, 0);
        if (cu_res != CUDA_SUCCESS)
        {
            GST_ERROR_OBJECT(self, "cuGraphicsEGLRegisterImage failed: %d", cu_res);
            eglDestroyImageKHR(self->egl_display, egl_image);
            close(dmabuf_fd);
            gbm_bo_destroy(bo);
            return GST_FLOW_ERROR;
        }

        CUeglFrame out_frame;
        cu_res = cuGraphicsResourceGetMappedEglFrame(&out_frame, res, 0, 0);
        if (cu_res != CUDA_SUCCESS)
        {
            GST_ERROR_OBJECT(self, "cuGraphicsResourceGetMappedEglFrame failed: %d", cu_res);
            cuGraphicsUnregisterResource(res);
            eglDestroyImageKHR(self->egl_display, egl_image);
            close(dmabuf_fd);
            gbm_bo_destroy(bo);
            return GST_FLOW_ERROR;
        }

        /* Map input CUDA buffer to get device pointer */
        GstMapInfo in_map;
        if (!gst_buffer_map(inbuf, &in_map, GST_MAP_READ | GST_MAP_CUDA))
        {
            GST_ERROR_OBJECT(self, "gst_buffer_map(CUDA) failed");
            cuGraphicsUnregisterResource(res);
            eglDestroyImageKHR(self->egl_display, egl_image);
            close(dmabuf_fd);
            gbm_bo_destroy(bo);
            return GST_FLOW_ERROR;
        }

        const uint8_t *in_base = (const uint8_t *)in_map.data;
        const void *in_y = (const void *)in_base;
        const void *in_uv = (const void *)(in_base + uv_offset_in);

        /* Copy Y plane */
        cu_res = copy_plane_to_eglframe(in_y, (size_t)y_stride_in,
                                        &out_frame, 0,
                                        (size_t)width, (size_t)height);
        if (cu_res != CUDA_SUCCESS)
        {
            GST_ERROR_OBJECT(self, "copy Y failed: %d", cu_res);
            gst_buffer_unmap(inbuf, &in_map);
            cuGraphicsUnregisterResource(res);
            eglDestroyImageKHR(self->egl_display, egl_image);
            close(dmabuf_fd);
            gbm_bo_destroy(bo);
            return GST_FLOW_ERROR;
        }

        /* Copy interleaved UV plane (height/2 rows, width bytes per row) */
        cu_res = copy_plane_to_eglframe(in_uv, (size_t)uv_stride_in,
                                        &out_frame, 1,
                                        (size_t)width, (size_t)(height / 2));
        if (cu_res != CUDA_SUCCESS)
        {
            GST_ERROR_OBJECT(self, "copy UV failed: %d", cu_res);
            gst_buffer_unmap(inbuf, &in_map);
            cuGraphicsUnregisterResource(res);
            eglDestroyImageKHR(self->egl_display, egl_image);
            close(dmabuf_fd);
            gbm_bo_destroy(bo);
            return GST_FLOW_ERROR;
        }

        gst_buffer_unmap(inbuf, &in_map);

        cuCtxSynchronize();
        cuGraphicsUnregisterResource(res);
        eglDestroyImageKHR(self->egl_display, egl_image);

        /* Wrap DMABUF into GstBuffer */
        if (!self->dmabuf_allocator)
            self->dmabuf_allocator = gst_dmabuf_allocator_new();

        gsize size = uv_offset_out + (gsize)uv_stride_out * (gsize)(height / 2);

        GstMemory *dmabuf_mem = gst_dmabuf_allocator_alloc(self->dmabuf_allocator, dmabuf_fd, size);
        if (!dmabuf_mem)
        {
            close(dmabuf_fd);
            gbm_bo_destroy(bo);
            return GST_FLOW_ERROR;
        }

        *outbuf = gst_buffer_new();
        gst_buffer_append_memory(*outbuf, dmabuf_mem);

        /* IMPORTANT: since your caps say format=DMA_DRM, use DMA_DRM meta format */
        {
            gsize offsets[4] = {0, uv_offset_out, 0, 0};
            gint strides[4] = {(gint)y_stride_out, (gint)uv_stride_out, 0, 0};

            gst_buffer_add_video_meta_full(*outbuf,
                                           GST_VIDEO_FRAME_FLAG_NONE,
                                           GST_VIDEO_FORMAT_DMA_DRM,
                                           width, height,
                                           2, offsets, strides);
        }

        gst_mini_object_set_qdata(GST_MINI_OBJECT(*outbuf),
                                  g_quark_from_static_string("gbm-bo"),
                                  bo, (GDestroyNotify)gbm_bo_destroy);

        GST_BUFFER_PTS(*outbuf) = GST_BUFFER_PTS(inbuf);
        GST_BUFFER_DTS(*outbuf) = GST_BUFFER_DTS(inbuf);
        GST_BUFFER_DURATION(*outbuf) = GST_BUFFER_DURATION(inbuf);

        return GST_FLOW_OK;
    }

    /* For CUDA NV12 input with XR24 output path:
     * 1. Allocate GBM buffer with NVIDIA tiled modifier (compositor-compatible)
     * 2. Import GBM DMABUF into CUDA as external memory
     * 3. Run NV12→BGRx CUDA kernel writing to imported memory
     * 4. Export GBM DMABUF to downstream
     */
    if (self->cuda_input)
    {
        GstMemory *mem = gst_buffer_peek_memory(inbuf, 0);

        if (!gst_is_cuda_memory(mem))
        {
            GST_ERROR_OBJECT(self, "Expected CUDA memory but got something else");
            return GST_FLOW_ERROR;
        }

        /* Get dimensions */
        guint width = GST_VIDEO_INFO_WIDTH(&self->cuda_info);
        guint height = GST_VIDEO_INFO_HEIGHT(&self->cuda_info);

        /* Get strides from video meta */
        GstVideoMeta *in_vmeta = gst_buffer_get_video_meta(inbuf);
        gint y_stride, uv_stride;
        gsize uv_offset;

        if (in_vmeta && in_vmeta->n_planes >= 2)
        {
            y_stride = in_vmeta->stride[0];
            uv_stride = in_vmeta->stride[1];
            uv_offset = in_vmeta->offset[1];
        }
        else
        {
            /* Fallback - assume packed layout */
            y_stride = width;
            uv_stride = width;
            uv_offset = width * height;
        }

        GST_DEBUG_OBJECT(self, "NV12 input: %ux%u, y_stride=%d, uv_stride=%d, uv_offset=%zu",
                         width, height, y_stride, uv_stride, uv_offset);

        /* Initialize GBM device and EGL if not done yet */
        if (!self->gbm)
        {
            self->drm_fd = open("/dev/dri/renderD129", O_RDWR | O_CLOEXEC);
            if (self->drm_fd < 0)
            {
                GST_ERROR_OBJECT(self, "Failed to open NVIDIA DRM device");
                return GST_FLOW_ERROR;
            }

            self->gbm = gbm_create_device(self->drm_fd);
            if (!self->gbm)
            {
                GST_ERROR_OBJECT(self, "Failed to create GBM device");
                close(self->drm_fd);
                self->drm_fd = -1;
                return GST_FLOW_ERROR;
            }

            GST_INFO_OBJECT(self, "Created GBM device for NVIDIA GPU");
        }

        /* Initialize EGL with GBM platform */
        if (!self->egl_initialized)
        {
            /* Get EGL display from GBM device using platform extension */
            PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT =
                (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");

            if (eglGetPlatformDisplayEXT)
            {
                self->egl_display = eglGetPlatformDisplayEXT(
                    EGL_PLATFORM_GBM_MESA, self->gbm, NULL);
            }
            else
            {
                /* Fallback to legacy approach */
                self->egl_display = eglGetDisplay((EGLNativeDisplayType)self->gbm);
            }

            if (self->egl_display == EGL_NO_DISPLAY)
            {
                GST_ERROR_OBJECT(self, "Failed to get EGL display: 0x%x", eglGetError());
                return GST_FLOW_ERROR;
            }

            EGLint major, minor;
            if (!eglInitialize(self->egl_display, &major, &minor))
            {
                GST_ERROR_OBJECT(self, "Failed to initialize EGL: 0x%x", eglGetError());
                self->egl_display = EGL_NO_DISPLAY;
                return GST_FLOW_ERROR;
            }

            GST_INFO_OBJECT(self, "EGL initialized: version %d.%d", major, minor);

            /* Create an EGL context (surfaceless) for CUDA interop */
            if (!eglBindAPI(EGL_OPENGL_ES_API))
            {
                GST_WARNING_OBJECT(self, "Failed to bind GLES API: 0x%x", eglGetError());
                /* Try OpenGL instead */
                eglBindAPI(EGL_OPENGL_API);
            }

            EGLint config_attribs[] = {
                EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                EGL_NONE};
            EGLConfig config;
            EGLint num_configs;
            if (!eglChooseConfig(self->egl_display, config_attribs, &config, 1, &num_configs) || num_configs == 0)
            {
                GST_WARNING_OBJECT(self, "Failed to get EGL config: 0x%x", eglGetError());
                /* Continue without context - might still work for some operations */
            }
            else
            {
                EGLint ctx_attribs[] = {
                    EGL_CONTEXT_CLIENT_VERSION, 2,
                    EGL_NONE};
                self->egl_context = eglCreateContext(self->egl_display, config, EGL_NO_CONTEXT, ctx_attribs);
                if (self->egl_context == EGL_NO_CONTEXT)
                {
                    GST_WARNING_OBJECT(self, "Failed to create EGL context: 0x%x", eglGetError());
                }
                else
                {
                    /* Make context current (surfaceless) */
                    if (!eglMakeCurrent(self->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, self->egl_context))
                    {
                        GST_WARNING_OBJECT(self, "Failed to make EGL context current: 0x%x", eglGetError());
                    }
                    else
                    {
                        GST_INFO_OBJECT(self, "Created and activated EGL context for CUDA interop");
                    }
                }
            }

            self->egl_initialized = TRUE;

            /* Initialize CUDA driver API */
            CUresult cu_res = cuInit(0);
            if (cu_res != CUDA_SUCCESS)
            {
                GST_ERROR_OBJECT(self, "Failed to init CUDA driver API: %d", cu_res);
                return GST_FLOW_ERROR;
            }
        }

        /* Create DMABUF allocator if not done yet */
        if (!self->dmabuf_allocator)
        {
            self->dmabuf_allocator = gst_dmabuf_allocator_new();
        }

        /* Allocate GBM buffer with negotiated NVIDIA tiled modifier */
        struct gbm_bo *bo = NULL;
        uint64_t modifier = self->negotiated_modifier;

        if (modifier != DRM_FORMAT_MOD_INVALID && modifier != 0)
        {
            uint64_t modifiers[] = {modifier};
            bo = gbm_bo_create_with_modifiers(
                self->gbm,
                width, height,
                GBM_FORMAT_XRGB8888, /* XR24 = BGRx */
                modifiers, 1);

            if (bo)
            {
                GST_DEBUG_OBJECT(self, "GBM: Created buffer with modifier 0x%016lx", modifier);
            }
        }

        /* Fallback to LINEAR if tiled creation failed */
        if (!bo)
        {
            bo = gbm_bo_create(
                self->gbm,
                width, height,
                GBM_FORMAT_XRGB8888,
                GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);

            if (bo)
            {
                GST_WARNING_OBJECT(self, "GBM: Fell back to LINEAR buffer");
            }
        }

        if (!bo)
        {
            GST_ERROR_OBJECT(self, "Failed to create GBM buffer");
            return GST_FLOW_ERROR;
        }

        int dmabuf_fd = gbm_bo_get_fd(bo);
        if (dmabuf_fd < 0)
        {
            GST_ERROR_OBJECT(self, "Failed to get DMABUF fd from GBM bo");
            gbm_bo_destroy(bo);
            return GST_FLOW_ERROR;
        }

        guint gbm_stride = gbm_bo_get_stride(bo);
        gsize gbm_size = (gsize)gbm_stride * height;

        GST_DEBUG_OBJECT(self, "GBM buffer: %ux%u, stride=%u, size=%zu, fd=%d",
                         width, height, gbm_stride, gbm_size, dmabuf_fd);

        /* Create EGLImage from GBM buffer for CUDA interop */
        PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR =
            (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
        PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR =
            (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");

        if (!eglCreateImageKHR || !eglDestroyImageKHR)
        {
            GST_ERROR_OBJECT(self, "EGL_KHR_image extension not available");
            close(dmabuf_fd);
            gbm_bo_destroy(bo);
            return GST_FLOW_ERROR;
        }

        /* Get actual modifier from GBM buffer */
        uint64_t actual_modifier = gbm_bo_get_modifier(bo);

        /* Create EGLImage from DMABUF using EGL_LINUX_DMA_BUF_EXT */
        EGLint attribs[] = {
            EGL_WIDTH, (EGLint)width,
            EGL_HEIGHT, (EGLint)height,
            EGL_LINUX_DRM_FOURCC_EXT, GBM_FORMAT_XRGB8888,
            EGL_DMA_BUF_PLANE0_FD_EXT, dmabuf_fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)gbm_stride,
            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(actual_modifier & 0xFFFFFFFF),
            EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(actual_modifier >> 32),
            EGL_NONE};

        GST_DEBUG_OBJECT(self, "Creating EGLImage with modifier 0x%016lx, fd=%d, stride=%u",
                         actual_modifier, dmabuf_fd, gbm_stride);

        EGLImage egl_image = eglCreateImageKHR(
            self->egl_display,
            EGL_NO_CONTEXT,
            EGL_LINUX_DMA_BUF_EXT,
            NULL, /* No client buffer for DMA_BUF */
            attribs);

        if (egl_image == EGL_NO_IMAGE)
        {
            GST_ERROR_OBJECT(self, "Failed to create EGLImage from DMABUF: 0x%x", eglGetError());
            close(dmabuf_fd);
            gbm_bo_destroy(bo);
            return GST_FLOW_ERROR;
        }

        GST_DEBUG_OBJECT(self, "Created EGLImage from DMABUF");

        /* Get CUDA device matching the GPU */
        CUdevice cu_device;
        CUresult cu_res = cuDeviceGet(&cu_device, 0);
        if (cu_res != CUDA_SUCCESS)
        {
            GST_ERROR_OBJECT(self, "Failed to get CUDA device: %d", cu_res);
            eglDestroyImageKHR(self->egl_display, egl_image);
            close(dmabuf_fd);
            gbm_bo_destroy(bo);
            return GST_FLOW_ERROR;
        }

        /* Create CUDA context if needed */
        CUcontext cu_context;
        cu_res = cuCtxGetCurrent(&cu_context);
        if (cu_res != CUDA_SUCCESS || cu_context == NULL)
        {
            cu_res = cuCtxCreate(&cu_context, 0, cu_device);
            if (cu_res != CUDA_SUCCESS)
            {
                GST_ERROR_OBJECT(self, "Failed to create CUDA context: %d", cu_res);
                eglDestroyImageKHR(self->egl_display, egl_image);
                close(dmabuf_fd);
                gbm_bo_destroy(bo);
                return GST_FLOW_ERROR;
            }
        }

        /* Register EGLImage with CUDA for write access
         * Note: flags=0 for default read/write access */
        CUgraphicsResource cuda_resource = NULL;
        cu_res = cuGraphicsEGLRegisterImage(
            &cuda_resource,
            egl_image,
            0); /* CU_GRAPHICS_REGISTER_FLAGS_NONE */

        if (cu_res != CUDA_SUCCESS)
        {
            GST_ERROR_OBJECT(self, "cuGraphicsEGLRegisterImage failed: %d", cu_res);
            eglDestroyImageKHR(self->egl_display, egl_image);
            close(dmabuf_fd);
            gbm_bo_destroy(bo);
            return GST_FLOW_ERROR;
        }

        GST_DEBUG_OBJECT(self, "Registered EGLImage with CUDA");

        /* Get the mapped EGL frame from CUDA */
        CUeglFrame cuda_frame;
        cu_res = cuGraphicsResourceGetMappedEglFrame(&cuda_frame, cuda_resource, 0, 0);

        if (cu_res != CUDA_SUCCESS)
        {
            GST_ERROR_OBJECT(self, "cuGraphicsResourceGetMappedEglFrame failed: %d", cu_res);
            cuGraphicsUnregisterResource(cuda_resource);
            eglDestroyImageKHR(self->egl_display, egl_image);
            close(dmabuf_fd);
            gbm_bo_destroy(bo);
            return GST_FLOW_ERROR;
        }

        /* Get the CUDA device pointer and pitch from the EGL frame */
        CUdeviceptr cuda_out_ptr = (CUdeviceptr)cuda_frame.frame.pPitch[0];
        guint cuda_pitch = cuda_frame.pitch;

        GST_DEBUG_OBJECT(self, "CUDA mapped EGL frame: ptr=%p, pitch=%u, gbm_stride=%u",
                         (void *)cuda_out_ptr, cuda_pitch, gbm_stride);

        /* Map NV12 input buffer for CUDA read */
        GstMapInfo in_map;
        if (!gst_buffer_map(inbuf, &in_map, GST_MAP_READ | GST_MAP_CUDA))
        {
            GST_ERROR_OBJECT(self, "Failed to map NV12 input for CUDA");
            cuGraphicsUnregisterResource(cuda_resource);
            eglDestroyImageKHR(self->egl_display, egl_image);
            close(dmabuf_fd);
            gbm_bo_destroy(bo);
            return GST_FLOW_ERROR;
        }

        /* Y plane is at start, UV plane is at offset */
        const void *y_plane = in_map.data;
        const void *uv_plane = (const uint8_t *)in_map.data + uv_offset;

        /* Use cuda_pitch for output stride (from EGL frame mapping) */
        GST_DEBUG_OBJECT(self, "Running NV12→BGRx CUDA kernel: y=%p, uv=%p, out=%p, bgrx_stride=%u",
                         y_plane, uv_plane, (void *)cuda_out_ptr, cuda_pitch);

        /* Run the NV12→BGRx conversion kernel writing to GBM buffer */
        int cuda_err = cuda_nv12_to_bgrx(
            y_plane,
            uv_plane,
            (void *)cuda_out_ptr,
            width, height,
            y_stride, uv_stride, cuda_pitch, /* Use CUDA pitch, not GBM stride */
            NULL                             /* default stream */
        );

        /* Sync to ensure kernel completed */
        cudaDeviceSynchronize();

        gst_buffer_unmap(inbuf, &in_map);

        /* Clean up CUDA/EGL resources (but keep GBM buffer and its DMABUF) */
        cuGraphicsUnregisterResource(cuda_resource);
        eglDestroyImageKHR(self->egl_display, egl_image);

        if (cuda_err != 0)
        {
            GST_ERROR_OBJECT(self, "CUDA NV12→BGRx kernel failed with error %d", cuda_err);
            close(dmabuf_fd);
            gbm_bo_destroy(bo);
            return GST_FLOW_ERROR;
        }

        GST_DEBUG_OBJECT(self, "NV12→BGRx GPU conversion complete!");

        /* Create output DMABUF buffer from GBM fd */
        /* The dmabuf_fd is still valid - EGL/CUDA didn't consume it */
        GstMemory *dmabuf_mem = gst_dmabuf_allocator_alloc(
            self->dmabuf_allocator, dmabuf_fd, gbm_size);

        if (!dmabuf_mem)
        {
            GST_ERROR_OBJECT(self, "Failed to create DMABUF memory");
            close(dmabuf_fd);
            gbm_bo_destroy(bo);
            return GST_FLOW_ERROR;
        }

        *outbuf = gst_buffer_new();
        gst_buffer_append_memory(*outbuf, dmabuf_mem);

        /* Add video meta for BGRx - use cuda_pitch for stride */
        {
            gsize offsets[4] = {0, 0, 0, 0};
            gint strides[4] = {(gint)cuda_pitch, 0, 0, 0};
            gst_buffer_add_video_meta_full(
                *outbuf,
                GST_VIDEO_FRAME_FLAG_NONE,
                GST_VIDEO_FORMAT_BGRx,
                width, height,
                1, /* 1 plane for BGRx */
                offsets,
                strides);
        }

        /* Copy timestamps */
        GST_BUFFER_PTS(*outbuf) = GST_BUFFER_PTS(inbuf);
        GST_BUFFER_DTS(*outbuf) = GST_BUFFER_DTS(inbuf);
        GST_BUFFER_DURATION(*outbuf) = GST_BUFFER_DURATION(inbuf);

        /* Keep GBM BO alive with buffer */
        gst_mini_object_set_qdata(GST_MINI_OBJECT(*outbuf),
                                  g_quark_from_static_string("gbm-bo"),
                                  bo, (GDestroyNotify)gbm_bo_destroy);

        g_print("GPU-ONLY TRANSFORM: %ux%u NV12→BGRx via GBM+CUDA (modifier=0x%016lx)\n",
                width, height, modifier);

        return GST_FLOW_OK;
    }

    /* For non-CUDA path, use GBM pool */
    if (!self->pool)
    {
        return GST_BASE_TRANSFORM_CLASS(gst_cuda_dmabuf_upload_parent_class)
            ->prepare_output_buffer(base, inbuf, outbuf);
    }

    GstFlowReturn ret = gst_buffer_pool_acquire_buffer(self->pool, outbuf, NULL);
    if (ret != GST_FLOW_OK)
    {
        GST_ERROR_OBJECT(self, "Failed to acquire buffer from pool: %s",
                         gst_flow_get_name(ret));
        return ret;
    }

    GST_LOG_OBJECT(self, "Acquired DMABUF buffer from pool");
    return GST_FLOW_OK;
}

static GstFlowReturn
gst_cuda_dmabuf_upload_transform(GstBaseTransform *base, GstBuffer *inbuf, GstBuffer *outbuf)
{
    GstCudaDmabufUpload *self = GST_CUDA_DMABUF_UPLOAD(base);

    /* For CUDA path with NV12 passthrough - work already done in prepare_output_buffer */
    if (self->cuda_input && self->nv12_output)
    {
        g_print("NV12 ZERO-COPY: %ux%u - direct passthrough to compositor!\n",
                GST_VIDEO_INFO_WIDTH(&self->cuda_info),
                GST_VIDEO_INFO_HEIGHT(&self->cuda_info));
        return GST_FLOW_OK;
    }

    /* For CUDA path with BGRx output - NV12→BGRx conversion was done in prepare_output_buffer */
    if (self->cuda_input)
    {
        g_print("GPU-ONLY TRANSFORM: %ux%u - NV12→BGRx on GPU, exported as DMABUF!\n",
                GST_VIDEO_INFO_WIDTH(&self->cuda_info),
                GST_VIDEO_INFO_HEIGHT(&self->cuda_info));
        return GST_FLOW_OK;
    }

    /* Non-CUDA path: copy from BGRx to DMABUF */
    guint width = GST_VIDEO_INFO_WIDTH(&self->info);
    guint height = GST_VIDEO_INFO_HEIGHT(&self->info);
    guint bytes_per_pixel = 4; /* BGRx */
    const guint row_bytes = width * bytes_per_pixel;

    /* Use GstVideoFrame to properly handle input buffer stride */
    GstVideoFrame in_frame;

    if (!gst_video_frame_map(&in_frame, &self->info, inbuf, GST_MAP_READ))
    {
        GST_ERROR_OBJECT(base, "Failed to map input buffer as video frame");
        return GST_FLOW_ERROR;
    }

    /* For output, we need to map manually since it's DMA_DRM format */
    GstMapInfo outmap;
    if (!gst_buffer_map(outbuf, &outmap, GST_MAP_WRITE))
    {
        gst_video_frame_unmap(&in_frame);
        GST_ERROR_OBJECT(base, "Failed to map output buffer for writing");
        return GST_FLOW_ERROR;
    }

    /* Get the output stride from video meta if available */
    GstVideoMeta *vmeta = gst_buffer_get_video_meta(outbuf);
    gint dst_stride;

    if (vmeta)
    {
        dst_stride = vmeta->stride[0];
    }
    else
    {
        dst_stride = width * bytes_per_pixel;
    }

    /* Get input stride from the video frame */
    gint src_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&in_frame, 0);
    const guint8 *srcp = (const guint8 *)GST_VIDEO_FRAME_PLANE_DATA(&in_frame, 0);
    guint8 *dstp = (guint8 *)outmap.data;

    g_print("TRANSFORM: %ux%u, src_stride=%d, dst_stride=%d, row_bytes=%u, outmap.size=%zu\n",
            width, height, src_stride, dst_stride, row_bytes, outmap.size);

    /* Copy line-by-line to respect stride */
    for (guint y = 0; y < height; y++)
    {
        memcpy(dstp, srcp, row_bytes);
        srcp += src_stride;
        dstp += dst_stride;
    }

    gst_buffer_unmap(outbuf, &outmap);
    gst_video_frame_unmap(&in_frame);

    return GST_FLOW_OK;
}

static gboolean
gst_cuda_dmabuf_upload_start(GstBaseTransform *trans)
{
    GstCudaDmabufUpload *self = GST_CUDA_DMABUF_UPLOAD(trans);

    GstContext *ctx = gst_element_get_context(GST_ELEMENT(trans), "gst.wayland.display");

    if (ctx)
    {
        const GstStructure *s = gst_context_get_structure(ctx);
        struct wl_display *display = NULL;

        gst_structure_get(s, "display", G_TYPE_POINTER, &display, NULL);

        self->wl_display = display;

        gst_context_unref(ctx);

        GST_INFO_OBJECT(self, "Got Wayland display context: %p", display);
    }
    else
    {
        GST_INFO_OBJECT(self, "No Wayland display context available (this is OK)");
    }

    return TRUE;
}

static void
gst_cuda_dmabuf_upload_finalize(GObject *object)
{
    GstCudaDmabufUpload *self = GST_CUDA_DMABUF_UPLOAD(object);

    if (self->pool)
    {
        gst_buffer_pool_set_active(self->pool, FALSE);
        gst_object_unref(self->pool);
        self->pool = NULL;
    }

    if (self->cuda_pool)
    {
        gst_buffer_pool_set_active(self->cuda_pool, FALSE);
        gst_object_unref(self->cuda_pool);
        self->cuda_pool = NULL;
    }

    if (self->cuda_bgrx_pool)
    {
        gst_buffer_pool_set_active(self->cuda_bgrx_pool, FALSE);
        gst_object_unref(self->cuda_bgrx_pool);
        self->cuda_bgrx_pool = NULL;
    }

    if (self->cuda_ctx)
    {
        gst_object_unref(self->cuda_ctx);
        self->cuda_ctx = NULL;
    }

    if (self->dmabuf_allocator)
    {
        gst_object_unref(self->dmabuf_allocator);
        self->dmabuf_allocator = NULL;
    }

    /* Clean up EGL before GBM */
    if (self->egl_display != EGL_NO_DISPLAY)
    {
        eglTerminate(self->egl_display);
        self->egl_display = EGL_NO_DISPLAY;
    }

    if (self->gbm)
    {
        gbm_device_destroy(self->gbm);
        self->gbm = NULL;
    }

    if (self->drm_fd >= 0)
    {
        close(self->drm_fd);
        self->drm_fd = -1;
    }

    G_OBJECT_CLASS(gst_cuda_dmabuf_upload_parent_class)->finalize(object);
}

static void
gst_cuda_dmabuf_upload_class_init(GstCudaDmabufUploadClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
    GstBaseTransformClass *base_class = GST_BASE_TRANSFORM_CLASS(klass);

    gobject_class->finalize = gst_cuda_dmabuf_upload_finalize;

    gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&sink_template));
    gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&src_template));

    gst_element_class_set_static_metadata(
        element_class,
        "CUDA → DMABUF Upload (GBM pool test)",
        "Filter/Video",
        "Allocates dmabuf via GBM and outputs video/x-raw",
        "Ericky");

    base_class->start = GST_DEBUG_FUNCPTR(gst_cuda_dmabuf_upload_start);
    base_class->set_caps = GST_DEBUG_FUNCPTR(gst_cuda_dmabuf_upload_set_caps);
    base_class->propose_allocation = GST_DEBUG_FUNCPTR(gst_cuda_dmabuf_upload_propose_allocation);
    base_class->decide_allocation = GST_DEBUG_FUNCPTR(gst_cuda_dmabuf_upload_decide_allocation);
    base_class->prepare_output_buffer = GST_DEBUG_FUNCPTR(gst_cuda_dmabuf_upload_prepare_output_buffer);
    base_class->transform = GST_DEBUG_FUNCPTR(gst_cuda_dmabuf_upload_transform);
    base_class->transform_caps =
        GST_DEBUG_FUNCPTR(gst_cuda_dmabuf_upload_transform_caps);

    base_class->passthrough_on_same_caps = FALSE;
}

static void
gst_cuda_dmabuf_upload_init(GstCudaDmabufUpload *self)
{
    gst_video_info_init(&self->info);
    gst_video_info_init(&self->cuda_info);
    self->wl_display = NULL;
    self->negotiated_modifier = DRM_FORMAT_MOD_INVALID;
    self->pool = NULL;
    self->cuda_pool = NULL;
    self->cuda_bgrx_pool = NULL;
    self->cuda_ctx = NULL;
    self->cuda_input = FALSE;
    self->dmabuf_allocator = NULL;
    self->drm_fd = -1;
    self->gbm = NULL;
    self->egl_display = EGL_NO_DISPLAY;
    self->egl_initialized = FALSE;
}
