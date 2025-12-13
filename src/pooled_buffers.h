/*
 * Pooled Buffer Management
 * Pre-allocated buffer pools for zero-copy video paths
 */

#ifndef __POOLED_BUFFERS_H__
#define __POOLED_BUFFERS_H__

#include "cuda_egl_interop.h"
#include <gst/gst.h>

G_BEGIN_DECLS

/* Default pool sizes */
#define POOLED_BUFFER_DEFAULT_SIZE 4

/**
 * PooledBufferPool - A pool of pre-allocated CUDA-EGL buffers
 */
typedef struct _PooledBufferPool
{
    CudaEglBuffer *buffers;
    guint pool_size;
    guint current_index; /* Round-robin index */

    /* Pool configuration */
    guint width;
    guint height;
    guint32 format;
    guint64 modifier;

    gboolean initialized;
} PooledBufferPool;

/**
 * Initialize a buffer pool with the specified parameters.
 *
 * @param pool Pool structure to initialize
 * @param ctx CUDA-EGL context for buffer allocation
 * @param pool_size Number of buffers in the pool
 * @param width Buffer width
 * @param height Buffer height
 * @param format GBM format (GBM_FORMAT_NV12 or GBM_FORMAT_XRGB8888)
 * @param modifier DRM modifier
 * @return TRUE on success
 */
gboolean pooled_buffer_pool_init(PooledBufferPool *pool,
                                 CudaEglContext *ctx,
                                 guint pool_size,
                                 guint width,
                                 guint height,
                                 guint32 format,
                                 guint64 modifier);

/**
 * Clean up a buffer pool and free all resources.
 */
void pooled_buffer_pool_cleanup(PooledBufferPool *pool, CudaEglContext *ctx);

/**
 * Acquire the next buffer from the pool (round-robin).
 * This will synchronize on the buffer's CUDA stream to ensure
 * any previous async operations are complete.
 *
 * @param pool The buffer pool
 * @return Pointer to the acquired buffer, or NULL on error
 */
CudaEglBuffer *pooled_buffer_pool_acquire(PooledBufferPool *pool);

/**
 * Check if the pool needs reinitialization due to dimension changes.
 */
gboolean pooled_buffer_pool_needs_reinit(PooledBufferPool *pool,
                                         guint width,
                                         guint height);

G_END_DECLS

#endif /* __POOLED_BUFFERS_H__ */
