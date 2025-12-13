/* SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2025 Ericky
 *
 * Buffer Transform Operations
 * Handles the actual buffer transform logic (NV12 passthrough, NV12→BGRx, BGRx copy)
 */

#ifndef __BUFFER_TRANSFORM_H__
#define __BUFFER_TRANSFORM_H__

#include "cuda_egl_interop.h"
#include "pooled_buffers.h"
#include <gst/gst.h>
#include <gst/video/video.h>

G_BEGIN_DECLS

/**
 * BufferTransformContext - Shared context for buffer transforms
 */
typedef struct _BufferTransformContext
{
    CudaEglContext *egl_ctx;
    GstAllocator *dmabuf_allocator;
    guint64 negotiated_modifier;
} BufferTransformContext;

/**
 * Initialize buffer transform context.
 * Ensures EGL context is initialized and dmabuf allocator is ready.
 *
 * @param btx Transform context to initialize
 * @param egl_ctx CUDA-EGL context (must be pre-initialized or will be initialized)
 * @param modifier Negotiated DRM modifier
 * @return TRUE on success
 */
gboolean buffer_transform_context_init(BufferTransformContext *btx,
                                       CudaEglContext *egl_ctx,
                                       guint64 modifier);

/**
 * NV12 zero-copy passthrough transform.
 * Copies NV12 planes from CUDA memory to DMA-BUF using async CUDA operations.
 *
 * @param btx Transform context
 * @param pool NV12 buffer pool
 * @param inbuf Input GstBuffer (CUDA NV12)
 * @param outbuf Output GstBuffer pointer (will be allocated)
 * @param info Video info for dimensions
 * @return GST_FLOW_OK on success
 */
GstFlowReturn buffer_transform_nv12_passthrough(BufferTransformContext *btx,
                                                PooledBufferPool *pool,
                                                GstBuffer *inbuf,
                                                GstBuffer **outbuf,
                                                const GstVideoInfo *info);

/**
 * NV12→BGRx conversion transform.
 * Converts CUDA NV12 to DMA-BUF XR24 using a CUDA kernel.
 *
 * @param btx Transform context
 * @param inbuf Input GstBuffer (CUDA NV12)
 * @param outbuf Output GstBuffer pointer (will be allocated)
 * @param info Video info for dimensions
 * @return GST_FLOW_OK on success
 */
GstFlowReturn buffer_transform_nv12_to_bgrx(BufferTransformContext *btx,
                                            GstBuffer *inbuf,
                                            GstBuffer **outbuf,
                                            const GstVideoInfo *info);

/**
 * BGRx CPU copy transform.
 * Copies BGRx from system memory to DMA-BUF.
 *
 * @param inbuf Input GstBuffer (system memory BGRx)
 * @param outbuf Output GstBuffer (from DMA-BUF pool)
 * @param info Video info for dimensions
 * @return GST_FLOW_OK on success
 */
GstFlowReturn buffer_transform_bgrx_copy(GstBuffer *inbuf,
                                         GstBuffer *outbuf,
                                         const GstVideoInfo *info);

G_END_DECLS

#endif /* __BUFFER_TRANSFORM_H__ */
