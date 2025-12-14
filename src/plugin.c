/* SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2025 Ericky
 */

#include <gst/gst.h>
#include "gstcudadmabufupload.h"

#define PACKAGE "gst-cuda-dmabuf"

/* Define the debug category here so it's available to all modules */
GST_DEBUG_CATEGORY(gst_cuda_dmabuf_upload_debug);

static gboolean
plugin_init(GstPlugin *plugin)
{
    /* Initialize debug category before registering elements */
    GST_DEBUG_CATEGORY_INIT(gst_cuda_dmabuf_upload_debug, "cudadmabufupload", 0,
                            "CUDA DMA-BUF upload element");

    return gst_element_register(
        plugin,
        "cudadmabufupload",
        GST_RANK_NONE,
        GST_TYPE_CUDA_DMABUF_UPLOAD);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    cudadmabuf,
    "CUDA DMABUF plugin",
    plugin_init,
    "0.1.0",
    "LGPL",
    "gst-cuda-dmabuf",
    "https://example.com")
