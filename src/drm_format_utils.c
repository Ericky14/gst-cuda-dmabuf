/*
 * DRM Format Utilities
 * Helper functions for parsing DRM formats and modifiers
 */

#include "drm_format_utils.h"
#include <string.h>
#include <stdlib.h>

guint64
drm_format_parse_modifier(const gchar *drm_format)
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

guint32
drm_format_get_fourcc(const gchar *drm_format)
{
    if (!drm_format)
        return 0;

    if (g_str_has_prefix(drm_format, "NV12"))
        return DRM_FORMAT_NV12;
    if (g_str_has_prefix(drm_format, "XR24"))
        return DRM_FORMAT_XRGB8888;
    if (g_str_has_prefix(drm_format, "AR24"))
        return DRM_FORMAT_ARGB8888;
    if (g_str_has_prefix(drm_format, "XB24"))
        return DRM_FORMAT_XBGR8888;
    if (g_str_has_prefix(drm_format, "AB24"))
        return DRM_FORMAT_ABGR8888;

    return 0;
}

gboolean
drm_format_is_nv12(const gchar *drm_format)
{
    return drm_format && g_str_has_prefix(drm_format, "NV12");
}

gboolean
drm_format_is_xr24(const gchar *drm_format)
{
    return drm_format && g_str_has_prefix(drm_format, "XR24");
}
