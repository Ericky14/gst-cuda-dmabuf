#include <gst/gst.h>
#include "gstcudadmabufupload.h"

#define PACKAGE "gst-cuda-dmabuf"

static gboolean
plugin_init(GstPlugin *plugin)
{
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
