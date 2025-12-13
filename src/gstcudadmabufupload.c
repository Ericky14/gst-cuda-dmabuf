#include "gstcudadmabufupload.h"
#include "gbm_dmabuf_pool.h"

#include <gst/video/video.h>
#include <wayland-client.h>
#include <drm/drm_fourcc.h>
#include <gbm.h>
#include <string.h>
#include <stdlib.h>

struct _GstCudaDmabufUpload
{
    GstBaseTransform parent;
    GstVideoInfo info;
    struct wl_display *wl_display;
    guint64 negotiated_modifier;
    GstBufferPool *pool;  /* Our GBM pool for output buffers */
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
    if (mod_str[0] == '0' && (mod_str[1] == 'x' || mod_str[1] == 'X')) {
        return strtoull(mod_str + 2, NULL, 16);
    }
    return strtoull(mod_str, NULL, 16);
}

/* Force a simple format for the first milestone */
static GstStaticPadTemplate sink_template =
    GST_STATIC_PAD_TEMPLATE(
        "sink",
        GST_PAD_SINK,
        GST_PAD_ALWAYS,
        GST_STATIC_CAPS("video/x-raw,"
                        "format=(string)BGRx,"
                        "width=(int)[1,MAX],"
                        "height=(int)[1,MAX],"
                        "framerate=(fraction)[0/1,MAX]"));

/* Advertise XR24 with AMD tiled modifiers required by waylandsink */
static GstStaticPadTemplate src_template =
    GST_STATIC_PAD_TEMPLATE(
        "src",
        GST_PAD_SRC,
        GST_PAD_ALWAYS,
        GST_STATIC_CAPS(
            /* AMD tiled DMA_DRM formats required by waylandsink */
            "video/x-raw(memory:DMABuf),"
            "format=(string)DMA_DRM,"
            "width=(int)[1,MAX],"
            "height=(int)[1,MAX],"
            "framerate=(fraction)[0/1,MAX],"
            "drm-format=(string){ "
            "XR24:0x0300000000606010, "  /* AMD_GFX9_64K_S */
            "XR24:0x0300000000606011, "
            "XR24:0x0300000000606012, "
            "XR24:0x0300000000606013, "
            "XR24:0x0300000000606014, "
            "XR24:0x0300000000606015, "
            "XR24:0x0300000000e08010, "  /* AMD alternate */
            "XR24:0x0300000000e08011, "
            "XR24:0x0300000000e08012, "
            "XR24:0x0300000000e08013, "
            "XR24:0x0300000000e08014, "
            "XR24:0x0300000000e08015 }"
            "; "
            /* Fallback: regular raw video via shared memory */
            "video/x-raw,"
            "format=(string)BGRx,"
            "width=(int)[1,MAX],"
            "height=(int)[1,MAX],"
            "framerate=(fraction)[0/1,MAX]"
            ));

static gboolean
gst_cuda_dmabuf_upload_set_caps(GstBaseTransform *base, GstCaps *incaps, GstCaps *outcaps)
{
    GstCudaDmabufUpload *self = GST_CUDA_DMABUF_UPLOAD(base);
    
    /* Parse output caps to get the negotiated drm-format */
    GstStructure *s = gst_caps_get_structure(outcaps, 0);
    const gchar *format = gst_structure_get_string(s, "format");
    const gchar *drm_format = gst_structure_get_string(s, "drm-format");
    
    /* Check if we're outputting DMA_DRM or regular video */
    if (format && g_strcmp0(format, "DMA_DRM") == 0 && drm_format) {
        self->negotiated_modifier = parse_drm_format_modifier(drm_format);
        GST_INFO_OBJECT(self, "Negotiated drm-format: %s (modifier: 0x%016lx)", 
                drm_format, self->negotiated_modifier);
    } else {
        /* Regular video format, no DMABUF */
        self->negotiated_modifier = DRM_FORMAT_MOD_INVALID;
        GST_INFO_OBJECT(self, "Negotiated regular video format: %s", format ? format : "unknown");
    }
    
    return gst_video_info_from_caps(&self->info, incaps);
}

static GstCaps *
gst_cuda_dmabuf_upload_transform_caps(
    GstBaseTransform *base,
    GstPadDirection direction,
    GstCaps *caps,
    GstCaps *filter)
{
    GstCaps *outcaps;

    if (direction == GST_PAD_SINK)
    {
        /* sink → src: Return our src template caps to let downstream pick a drm-format */
        outcaps = gst_static_pad_template_get_caps(&src_template);
        
        /* Copy width/height/framerate from input caps */
        GstCaps *tmp = gst_caps_copy(outcaps);
        gst_caps_unref(outcaps);
        outcaps = tmp;
        
        for (guint i = 0; i < gst_caps_get_size(outcaps); i++)
        {
            GstStructure *s = gst_caps_get_structure(outcaps, i);
            GstStructure *in_s = gst_caps_get_structure(caps, 0);
            
            const GValue *w = gst_structure_get_value(in_s, "width");
            const GValue *h = gst_structure_get_value(in_s, "height");
            const GValue *fr = gst_structure_get_value(in_s, "framerate");
            
            if (w) gst_structure_set_value(s, "width", w);
            if (h) gst_structure_set_value(s, "height", h);
            if (fr) gst_structure_set_value(s, "framerate", fr);
        }
    }
    else
    {
        /* src → sink: strip memory features and restore original format */
        outcaps = gst_caps_copy(caps);
        
        for (guint i = 0; i < gst_caps_get_size(outcaps); i++)
        {
            gst_caps_set_features(outcaps, i,
                                  gst_caps_features_new_any());

            GstStructure *s = gst_caps_get_structure(outcaps, i);
            gst_structure_set(s, "format", G_TYPE_STRING, "BGRx", NULL);
            gst_structure_remove_field(s, "drm-format");
        }
    }

    if (filter)
    {
        GstCaps *tmp = gst_caps_intersect_full(
            outcaps, filter, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref(outcaps);
        outcaps = tmp;
    }

    return outcaps;
}

static gboolean
gst_cuda_dmabuf_upload_decide_allocation(GstBaseTransform *base, GstQuery *query)
{
    GstCudaDmabufUpload *self = GST_CUDA_DMABUF_UPLOAD(base);

    /* Release old pool if any */
    if (self->pool) {
        gst_buffer_pool_set_active(self->pool, FALSE);
        gst_object_unref(self->pool);
        self->pool = NULL;
    }

    /* If not using DMABUF, let GstBaseTransform handle allocation */
    if (self->negotiated_modifier == DRM_FORMAT_MOD_INVALID) {
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
    if (!caps) {
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

static GstFlowReturn
gst_cuda_dmabuf_upload_prepare_output_buffer(GstBaseTransform *base, 
                                              GstBuffer *inbuf, 
                                              GstBuffer **outbuf)
{
    GstCudaDmabufUpload *self = GST_CUDA_DMABUF_UPLOAD(base);
    
    /* If not using DMABUF pool, let GstBaseTransform handle it */
    if (!self->pool) {
        return GST_BASE_TRANSFORM_CLASS(gst_cuda_dmabuf_upload_parent_class)
            ->prepare_output_buffer(base, inbuf, outbuf);
    }
    
    if (!self->pool) {
        GST_ERROR_OBJECT(self, "No buffer pool available");
        return GST_FLOW_ERROR;
    }
    
    GstFlowReturn ret = gst_buffer_pool_acquire_buffer(self->pool, outbuf, NULL);
    if (ret != GST_FLOW_OK) {
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
    
    GstMapInfo inmap, outmap;
    if (!gst_buffer_map(inbuf, &inmap, GST_MAP_READ)) {
        GST_ERROR_OBJECT(base, "Failed to map input buffer");
        return GST_FLOW_ERROR;
    }

    if (!gst_buffer_map(outbuf, &outmap, GST_MAP_WRITE))
    {
        gst_buffer_unmap(inbuf, &inmap);
        GST_ERROR_OBJECT(base, "Failed to map output buffer for writing");
        return GST_FLOW_ERROR;
    }

    /* Get video info for stride calculation */
    guint width = GST_VIDEO_INFO_WIDTH(&self->info);
    guint height = GST_VIDEO_INFO_HEIGHT(&self->info);
    guint bytes_per_pixel = 4; /* BGRx */

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

    /* Copy line-by-line to respect stride */
    const guint src_stride = width * bytes_per_pixel;
    const guint row_bytes = width * bytes_per_pixel;

    const guint8 *srcp = (const guint8 *)inmap.data;
    guint8 *dstp = (guint8 *)outmap.data;

    for (guint y = 0; y < height; y++)
    {
        memcpy(dstp, srcp, row_bytes);
        srcp += src_stride;
        dstp += dst_stride;
    }

    gst_buffer_unmap(outbuf, &outmap);
    gst_buffer_unmap(inbuf, &inmap);

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
    
    if (self->pool) {
        gst_buffer_pool_set_active(self->pool, FALSE);
        gst_object_unref(self->pool);
        self->pool = NULL;
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
    self->wl_display = NULL;
    self->negotiated_modifier = DRM_FORMAT_MOD_INVALID;
    self->pool = NULL;
}
