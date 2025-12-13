/* SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2025 Ericky
 *
 * DRM Format Utilities
 * Helper functions for parsing DRM formats and modifiers
 */

#ifndef __DRM_FORMAT_UTILS_H__
#define __DRM_FORMAT_UTILS_H__

#include <glib.h>
#include <drm/drm_fourcc.h>

G_BEGIN_DECLS

/**
 * Parse a drm-format string like "XR24:0x0300000000606010" to extract the modifier.
 *
 * @param drm_format The drm-format string from GStreamer caps
 * @return The modifier value, or DRM_FORMAT_MOD_INVALID if parsing fails
 */
guint64 drm_format_parse_modifier(const gchar *drm_format);

/**
 * Get the fourcc code from a drm-format string.
 *
 * @param drm_format The drm-format string (e.g., "NV12:0x...")
 * @return The DRM fourcc code (e.g., DRM_FORMAT_NV12), or 0 if unknown
 */
guint32 drm_format_get_fourcc(const gchar *drm_format);

/**
 * Check if a drm-format string represents NV12.
 */
gboolean drm_format_is_nv12(const gchar *drm_format);

/**
 * Check if a drm-format string represents XR24 (BGRx).
 */
gboolean drm_format_is_xr24(const gchar *drm_format);

G_END_DECLS

#endif /* __DRM_FORMAT_UTILS_H__ */
