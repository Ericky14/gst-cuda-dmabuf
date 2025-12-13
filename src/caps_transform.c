/*
 * Caps Transform Utilities
 * Handles GStreamer caps negotiation and transformation
 */

#include "caps_transform.h"
#include "drm_format_utils.h"

#define GST_USE_UNSTABLE_API
#include <gst/cuda/gstcuda.h>
#include <drm/drm_fourcc.h>
#include <string.h>

/* NV12 modifiers supported */
static const char *nv12_modifiers[] = {
    "NV12:0x0300000000606010", "NV12:0x0300000000606011",
    "NV12:0x0300000000606012", "NV12:0x0300000000606013",
    "NV12:0x0300000000606014", "NV12:0x0300000000606015",
    "NV12:0x0300000000e08010", "NV12:0x0300000000e08011",
    "NV12:0x0300000000e08012", "NV12:0x0300000000e08013",
    "NV12:0x0300000000e08014", "NV12:0x0300000000e08015",
    "NV12:0x0", "NV12:0x100000000000001", NULL};

/* XR24 modifiers supported */
static const char *xr24_modifiers[] = {
    "XR24:0x0300000000606010", "XR24:0x0300000000606011",
    "XR24:0x0300000000606012", "XR24:0x0300000000606013",
    "XR24:0x0300000000606014", "XR24:0x0300000000606015", NULL};

void caps_transform_add_drm(GstCaps *caps, const gchar *drm_format,
                            const GValue *width, const GValue *height,
                            const GValue *framerate)
{
    GstCaps *tmp = gst_caps_new_simple(
        "video/x-raw",
        "format", G_TYPE_STRING, "DMA_DRM",
        "drm-format", G_TYPE_STRING, drm_format,
        NULL);
    gst_caps_set_features(tmp, 0, gst_caps_features_new("memory:DMABuf", NULL));

    GstStructure *s = gst_caps_get_structure(tmp, 0);
    if (width)
        gst_structure_set_value(s, "width", width);
    if (height)
        gst_structure_set_value(s, "height", height);
    if (framerate)
        gst_structure_set_value(s, "framerate", framerate);

    gst_caps_append(caps, tmp);
}

GstCaps *
caps_transform_sink_to_src(GstCaps *caps)
{
    GstCapsFeatures *features = gst_caps_get_features(caps, 0);
    gboolean is_cuda = gst_caps_features_contains(features, GST_CAPS_FEATURE_MEMORY_CUDA_MEMORY);
    GstStructure *in_s = gst_caps_get_structure(caps, 0);
    const gchar *in_format = gst_structure_get_string(in_s, "format");
    const GValue *w = gst_structure_get_value(in_s, "width");
    const GValue *h = gst_structure_get_value(in_s, "height");
    const GValue *fr = gst_structure_get_value(in_s, "framerate");

    GstCaps *outcaps = gst_caps_new_empty();

    if (is_cuda && g_strcmp0(in_format, "NV12") == 0)
    {
        /* CUDA NV12 → NV12 DMA-BUF (preferred) */
        for (int i = 0; nv12_modifiers[i]; i++)
            caps_transform_add_drm(outcaps, nv12_modifiers[i], w, h, fr);

        /* Fallback to XR24 with conversion */
        for (int i = 0; xr24_modifiers[i]; i++)
            caps_transform_add_drm(outcaps, xr24_modifiers[i], w, h, fr);
    }
    else if (g_strcmp0(in_format, "BGRx") == 0)
    {
        /* BGRx → XR24 DMA-BUF */
        for (int i = 0; xr24_modifiers[i] && i < 3; i++)
            caps_transform_add_drm(outcaps, xr24_modifiers[i], w, h, fr);
    }

    return outcaps;
}

/* Helper to add CUDA NV12 caps */
static void
add_cuda_nv12_caps(GstCaps *outcaps, const GValue *w, const GValue *h, const GValue *fr)
{
    GstCaps *tmp = gst_caps_new_simple(
        "video/x-raw", "format", G_TYPE_STRING, "NV12", NULL);
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

/* Helper to add BGRx caps */
static void
add_bgrx_caps(GstCaps *outcaps, const GValue *w, const GValue *h, const GValue *fr)
{
    GstCaps *tmp = gst_caps_new_simple(
        "video/x-raw", "format", G_TYPE_STRING, "BGRx", NULL);
    GstStructure *s = gst_caps_get_structure(tmp, 0);
    if (w)
        gst_structure_set_value(s, "width", w);
    if (h)
        gst_structure_set_value(s, "height", h);
    if (fr)
        gst_structure_set_value(s, "framerate", fr);
    gst_caps_append(outcaps, tmp);
}

GstCaps *
caps_transform_src_to_sink(GstCaps *caps)
{
    GstCaps *outcaps = gst_caps_new_empty();

    for (guint i = 0; i < gst_caps_get_size(caps); i++)
    {
        GstStructure *out_s = gst_caps_get_structure(caps, i);
        GstCapsFeatures *features = gst_caps_get_features(caps, i);
        const gchar *format = gst_structure_get_string(out_s, "format");
        const GValue *w = gst_structure_get_value(out_s, "width");
        const GValue *h = gst_structure_get_value(out_s, "height");
        const GValue *fr = gst_structure_get_value(out_s, "framerate");

        gboolean is_dmabuf = features &&
                             gst_caps_features_contains(features, "memory:DMABuf");

        if (format && g_strcmp0(format, "DMA_DRM") == 0 && is_dmabuf)
        {
            const GValue *drm_val = gst_structure_get_value(out_s, "drm-format");
            gboolean has_nv12 = FALSE, has_xr24 = FALSE;

            if (drm_val)
            {
                if (G_VALUE_HOLDS_STRING(drm_val))
                {
                    const gchar *drm = g_value_get_string(drm_val);
                    has_nv12 = drm_format_is_nv12(drm);
                    has_xr24 = drm_format_is_xr24(drm);
                }
                else if (GST_VALUE_HOLDS_LIST(drm_val))
                {
                    for (guint j = 0; j < gst_value_list_get_size(drm_val); j++)
                    {
                        const GValue *v = gst_value_list_get_value(drm_val, j);
                        if (G_VALUE_HOLDS_STRING(v))
                        {
                            const gchar *drm = g_value_get_string(v);
                            if (drm_format_is_nv12(drm))
                                has_nv12 = TRUE;
                            if (drm_format_is_xr24(drm))
                                has_xr24 = TRUE;
                        }
                    }
                }
            }

            if (has_nv12)
                add_cuda_nv12_caps(outcaps, w, h, fr);

            if (has_xr24)
            {
                /* XR24 can come from CUDA NV12 or regular BGRx */
                add_cuda_nv12_caps(outcaps, w, h, fr);
                add_bgrx_caps(outcaps, w, h, fr);
            }
        }
        else if (format && g_strcmp0(format, "BGRx") == 0)
        {
            add_bgrx_caps(outcaps, w, h, fr);
        }
    }

    return outcaps;
}
