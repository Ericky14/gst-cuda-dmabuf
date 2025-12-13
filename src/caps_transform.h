/*
 * Caps Transform Utilities
 * Handles GStreamer caps negotiation and transformation
 */

#ifndef __CAPS_TRANSFORM_H__
#define __CAPS_TRANSFORM_H__

#include <gst/gst.h>
#include <gst/video/video.h>

G_BEGIN_DECLS

/**
 * Add DMA-BUF caps with the given DRM format and dimensions.
 *
 * @param caps Caps to append to
 * @param drm_format DRM format string (e.g., "NV12:0x0300000000606010")
 * @param width Width GValue (or NULL)
 * @param height Height GValue (or NULL)
 * @param framerate Framerate GValue (or NULL)
 */
void caps_transform_add_drm(GstCaps *caps, const gchar *drm_format,
                            const GValue *width, const GValue *height,
                            const GValue *framerate);

/**
 * Transform sink caps to source caps.
 * CUDA NV12 → NV12 DMA-BUF (preferred) or XR24 DMA-BUF (fallback)
 * BGRx → XR24 DMA-BUF
 *
 * @param caps Input caps from sink
 * @return Transformed caps for source (caller owns reference)
 */
GstCaps *caps_transform_sink_to_src(GstCaps *caps);

/**
 * Transform source caps to sink caps (reverse direction).
 * NV12 DMA-BUF → CUDA NV12
 * XR24 DMA-BUF → CUDA NV12 or BGRx
 *
 * @param caps Input caps from source
 * @return Transformed caps for sink (caller owns reference)
 */
GstCaps *caps_transform_src_to_sink(GstCaps *caps);

/**
 * Get the default sink pad template caps.
 *
 * @return Static caps for sink pad (do not free)
 */
GstCaps *caps_transform_get_sink_template(void);

G_END_DECLS

#endif /* __CAPS_TRANSFORM_H__ */
