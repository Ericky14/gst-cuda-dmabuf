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
 * Semi-planar 4:2:0 zero-copy passthrough transform.
 * Copies Y+UV planes from CUDA memory to DMA-BUF using async CUDA operations.
 * Works for both NV12 (8-bit) and P010 (10-bit).
 *
 * @param btx Transform context
 * @param pool Buffer pool (NV12 or P010 format)
 * @param inbuf Input GstBuffer (CUDA NV12 or P010_10LE)
 * @param outbuf Output GstBuffer pointer (will be allocated)
 * @param info Video info for dimensions
 * @param is_p010 TRUE for P010 (16-bit samples), FALSE for NV12 (8-bit)
 * @return GST_FLOW_OK on success
 */
GstFlowReturn buffer_transform_semi_planar_passthrough(BufferTransformContext *btx,
                                                       PooledBufferPool *pool,
                                                       GstBuffer *inbuf,
                                                       GstBuffer **outbuf,
                                                       const GstVideoInfo *info,
                                                       gboolean is_p010);

/**
 * CUDA memory direct DMA-BUF export (zero-copy, no intermediate buffer).
 * Exports CUDA MMAP-allocated memory directly as DMA-BUF.
 * Used for P010 where GBM doesn't support the format.
 *
 * @param btx Transform context (needs dmabuf_allocator)
 * @param inbuf Input GstBuffer (CUDA MMAP memory)
 * @param outbuf Output GstBuffer pointer (will be allocated)
 * @param info Video info for dimensions
 * @return GST_FLOW_OK on success
 */
GstFlowReturn buffer_transform_cuda_export(BufferTransformContext *btx,
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
