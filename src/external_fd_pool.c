/* SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2025 Ericky
 *
 * External FD Pool — CUDA import of Vulkan-exported DMA-BUF FDs
 */

#include "external_fd_pool.h"
#include <string.h>
#include <unistd.h>

gboolean
external_fd_buffer_import(ExternalFdBuffer *buf,
                          int y_fd, gsize y_size, guint y_stride,
                          int uv_fd, gsize uv_size, guint uv_stride)
{
    memset(buf, 0, sizeof(*buf));
    buf->y_fd = y_fd;
    buf->uv_fd = uv_fd;
    buf->y_size = y_size;
    buf->uv_size = uv_size;
    buf->y_stride = y_stride;
    buf->uv_stride = uv_stride;

    CUresult cu_res;

    /* Import Y plane FD as CUDA external memory.
     * Use OPAQUE_FD for Vulkan-exported DMA-BUFs — the NVIDIA driver
     * handles DMA-BUF FDs through this handle type. We dup() the fd
     * because CUDA takes ownership of the fd passed to it. */
    CUDA_EXTERNAL_MEMORY_HANDLE_DESC y_mem_desc;
    memset(&y_mem_desc, 0, sizeof(y_mem_desc));
    y_mem_desc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
    y_mem_desc.handle.fd = dup(y_fd);
    y_mem_desc.size = y_size;
    y_mem_desc.flags = 0;

    if (y_mem_desc.handle.fd < 0)
    {
        g_warning("external_fd_buffer_import: failed to dup Y fd %d", y_fd);
        return FALSE;
    }

    cu_res = cuImportExternalMemory(&buf->y_ext_mem, &y_mem_desc);
    if (cu_res != CUDA_SUCCESS)
    {
        g_warning("cuImportExternalMemory (Y) failed: %d (fd=%d, size=%zu)",
                  cu_res, y_fd, y_size);
        close(y_mem_desc.handle.fd);
        return FALSE;
    }

    /* Map Y external memory to a device pointer */
    CUDA_EXTERNAL_MEMORY_BUFFER_DESC y_buf_desc;
    memset(&y_buf_desc, 0, sizeof(y_buf_desc));
    y_buf_desc.offset = 0;
    y_buf_desc.size = y_size;
    y_buf_desc.flags = 0;

    cu_res = cuExternalMemoryGetMappedBuffer(&buf->y_devptr, buf->y_ext_mem, &y_buf_desc);
    if (cu_res != CUDA_SUCCESS)
    {
        g_warning("cuExternalMemoryGetMappedBuffer (Y) failed: %d", cu_res);
        cuDestroyExternalMemory(buf->y_ext_mem);
        buf->y_ext_mem = NULL;
        return FALSE;
    }

    /* Import UV plane FD */
    CUDA_EXTERNAL_MEMORY_HANDLE_DESC uv_mem_desc;
    memset(&uv_mem_desc, 0, sizeof(uv_mem_desc));
    uv_mem_desc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
    uv_mem_desc.handle.fd = dup(uv_fd);
    uv_mem_desc.size = uv_size;
    uv_mem_desc.flags = 0;

    if (uv_mem_desc.handle.fd < 0)
    {
        g_warning("external_fd_buffer_import: failed to dup UV fd %d", uv_fd);
        cuDestroyExternalMemory(buf->y_ext_mem);
        buf->y_ext_mem = NULL;
        return FALSE;
    }

    cu_res = cuImportExternalMemory(&buf->uv_ext_mem, &uv_mem_desc);
    if (cu_res != CUDA_SUCCESS)
    {
        g_warning("cuImportExternalMemory (UV) failed: %d (fd=%d, size=%zu)",
                  cu_res, uv_fd, uv_size);
        close(uv_mem_desc.handle.fd);
        cuDestroyExternalMemory(buf->y_ext_mem);
        buf->y_ext_mem = NULL;
        return FALSE;
    }

    /* Map UV external memory */
    CUDA_EXTERNAL_MEMORY_BUFFER_DESC uv_buf_desc;
    memset(&uv_buf_desc, 0, sizeof(uv_buf_desc));
    uv_buf_desc.offset = 0;
    uv_buf_desc.size = uv_size;
    uv_buf_desc.flags = 0;

    cu_res = cuExternalMemoryGetMappedBuffer(&buf->uv_devptr, buf->uv_ext_mem, &uv_buf_desc);
    if (cu_res != CUDA_SUCCESS)
    {
        g_warning("cuExternalMemoryGetMappedBuffer (UV) failed: %d", cu_res);
        cuDestroyExternalMemory(buf->uv_ext_mem);
        buf->uv_ext_mem = NULL;
        cuDestroyExternalMemory(buf->y_ext_mem);
        buf->y_ext_mem = NULL;
        return FALSE;
    }

    /* Create CUDA stream for async copy */
    cu_res = cuStreamCreate(&buf->cuda_stream, CU_STREAM_NON_BLOCKING);
    if (cu_res != CUDA_SUCCESS)
    {
        g_warning("cuStreamCreate failed: %d", cu_res);
        cuDestroyExternalMemory(buf->uv_ext_mem);
        cuDestroyExternalMemory(buf->y_ext_mem);
        buf->uv_ext_mem = NULL;
        buf->y_ext_mem = NULL;
        return FALSE;
    }

    buf->initialized = TRUE;
    g_info("external_fd_buffer_import: Y fd=%d size=%zu stride=%u, UV fd=%d size=%zu stride=%u",
           y_fd, y_size, y_stride, uv_fd, uv_size, uv_stride);

    return TRUE;
}

void external_fd_buffer_release(ExternalFdBuffer *buf)
{
    if (!buf || !buf->initialized)
        return;

    if (buf->cuda_stream)
    {
        cuStreamSynchronize(buf->cuda_stream);
        cuStreamDestroy(buf->cuda_stream);
        buf->cuda_stream = NULL;
    }

    if (buf->y_ext_mem)
    {
        cuDestroyExternalMemory(buf->y_ext_mem);
        buf->y_ext_mem = NULL;
    }
    if (buf->uv_ext_mem)
    {
        cuDestroyExternalMemory(buf->uv_ext_mem);
        buf->uv_ext_mem = NULL;
    }

    buf->y_devptr = 0;
    buf->uv_devptr = 0;
    buf->initialized = FALSE;

    /* Note: we do NOT close y_fd / uv_fd — they're owned by the caller */
}

gboolean
external_fd_pool_init(ExternalFdPool *pool,
                      guint width, guint height,
                      gboolean is_p010)
{
    memset(pool, 0, sizeof(*pool));
    pool->width = width;
    pool->height = height;
    pool->is_p010 = is_p010;
    pool->initialized = TRUE;
    return TRUE;
}

gboolean
external_fd_pool_add(ExternalFdPool *pool,
                     int y_fd, gsize y_size, guint y_stride,
                     int uv_fd, gsize uv_size, guint uv_stride)
{
    if (!pool || !pool->initialized)
        return FALSE;

    if (pool->count >= EXTERNAL_FD_POOL_MAX_BUFFERS)
    {
        g_warning("external_fd_pool_add: pool full (%u/%d)",
                  pool->count, EXTERNAL_FD_POOL_MAX_BUFFERS);
        return FALSE;
    }

    if (!external_fd_buffer_import(&pool->buffers[pool->count],
                                   y_fd, y_size, y_stride,
                                   uv_fd, uv_size, uv_stride))
    {
        return FALSE;
    }

    pool->count++;
    g_info("external_fd_pool_add: buffer %u added (total: %u)", pool->count - 1, pool->count);
    return TRUE;
}

ExternalFdBuffer *
external_fd_pool_acquire(ExternalFdPool *pool)
{
    if (!pool || !pool->initialized || pool->count == 0)
        return NULL;

    ExternalFdBuffer *buf = &pool->buffers[pool->current_index];

    /* Sync on the stream to ensure any previous copy into this buffer is done */
    if (buf->cuda_stream)
        cuStreamSynchronize(buf->cuda_stream);

    pool->current_index = (pool->current_index + 1) % pool->count;
    return buf;
}

void external_fd_pool_cleanup(ExternalFdPool *pool)
{
    if (!pool || !pool->initialized)
        return;

    for (guint i = 0; i < pool->count; i++)
        external_fd_buffer_release(&pool->buffers[i]);

    pool->count = 0;
    pool->current_index = 0;
    pool->initialized = FALSE;
}
