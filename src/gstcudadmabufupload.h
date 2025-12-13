/* SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2025 Ericky
 */

#pragma once

#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>

G_BEGIN_DECLS

#define GST_TYPE_CUDA_DMABUF_UPLOAD (gst_cuda_dmabuf_upload_get_type())

G_DECLARE_FINAL_TYPE(
    GstCudaDmabufUpload,
    gst_cuda_dmabuf_upload,
    GST,
    CUDA_DMABUF_UPLOAD,
    GstBaseTransform)

G_END_DECLS
