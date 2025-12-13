/*
 * Pooled Buffer Management
 * Pre-allocated buffer pools for zero-copy video paths
 */

#include "pooled_buffers.h"
#include <string.h>

gboolean
pooled_buffer_pool_init(PooledBufferPool *pool,
                        CudaEglContext *ctx,
                        guint pool_size,
                        guint width,
                        guint height,
                        guint32 format,
                        guint64 modifier)
{
    g_return_val_if_fail(pool != NULL, FALSE);
    g_return_val_if_fail(ctx != NULL && ctx->initialized, FALSE);
    g_return_val_if_fail(pool_size > 0, FALSE);

    memset(pool, 0, sizeof(PooledBufferPool));

    pool->buffers = g_new0(CudaEglBuffer, pool_size);
    if (!pool->buffers)
    {
        g_warning("Failed to allocate buffer pool array");
        return FALSE;
    }

    pool->pool_size = pool_size;
    pool->width = width;
    pool->height = height;
    pool->format = format;
    pool->modifier = modifier;

    g_info("Initializing buffer pool: %ux%u, format=0x%x, modifier=0x%016lx, size=%u",
           width, height, format, modifier, pool_size);

    for (guint i = 0; i < pool_size; i++)
    {
        if (!cuda_egl_buffer_alloc(ctx, &pool->buffers[i],
                                   width, height, format, modifier))
        {
            g_warning("Failed to allocate buffer %u in pool", i);

            /* Clean up already allocated buffers */
            for (guint j = 0; j < i; j++)
            {
                cuda_egl_buffer_free(ctx, &pool->buffers[j]);
            }
            g_free(pool->buffers);
            pool->buffers = NULL;
            return FALSE;
        }

        g_debug("Pool buffer %u: fd=%d, strides=[%u,%u], offsets=[%u,%u]",
                i, pool->buffers[i].dmabuf_fd,
                pool->buffers[i].strides[0], pool->buffers[i].strides[1],
                pool->buffers[i].offsets[0], pool->buffers[i].offsets[1]);
    }

    pool->current_index = 0;
    pool->initialized = TRUE;

    g_info("Buffer pool initialized with %u buffers", pool_size);
    return TRUE;
}

void pooled_buffer_pool_cleanup(PooledBufferPool *pool, CudaEglContext *ctx)
{
    if (!pool || !pool->buffers)
        return;

    g_debug("Cleaning up buffer pool");

    for (guint i = 0; i < pool->pool_size; i++)
    {
        cuda_egl_buffer_free(ctx, &pool->buffers[i]);
    }

    g_free(pool->buffers);
    pool->buffers = NULL;
    pool->pool_size = 0;
    pool->initialized = FALSE;
}

CudaEglBuffer *
pooled_buffer_pool_acquire(PooledBufferPool *pool)
{
    g_return_val_if_fail(pool != NULL && pool->initialized, NULL);
    g_return_val_if_fail(pool->buffers != NULL, NULL);

    CudaEglBuffer *buf = &pool->buffers[pool->current_index];
    pool->current_index = (pool->current_index + 1) % pool->pool_size;

    /* Synchronize on this buffer's stream to ensure previous operations complete.
     * Due to round-robin, we're pool_size frames behind, giving time for async completion. */
    if (buf->cuda_stream)
    {
        CUresult cu_res = cuStreamSynchronize(buf->cuda_stream);
        if (cu_res != CUDA_SUCCESS)
        {
            g_warning("cuStreamSynchronize failed: %d", cu_res);
        }
    }

    buf->in_use = TRUE;
    return buf;
}

gboolean
pooled_buffer_pool_needs_reinit(PooledBufferPool *pool,
                                guint width,
                                guint height)
{
    if (!pool || !pool->initialized)
        return TRUE;

    return (pool->width != width || pool->height != height);
}
