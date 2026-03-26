/* SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2025 Ericky
 *
 * External FD Pool — Manages Vulkan-exported DMA-BUF FDs for CUDA interop
 *
 * Instead of GBM→EGL→CUDA (the internal allocation path), this accepts
 * DMA-BUF file descriptors from an external allocator (e.g., Vulkan) and
 * imports them into CUDA via cuImportExternalMemory. This enables true
 * zero-copy: Vulkan renders from the same memory CUDA writes to.
 */

#ifndef __EXTERNAL_FD_POOL_H__
#define __EXTERNAL_FD_POOL_H__

#include <glib.h>
#include <cuda.h>

G_BEGIN_DECLS

#define EXTERNAL_FD_POOL_MAX_BUFFERS 16

/**
 * ExternalFdBuffer - A single imported external DMA-BUF buffer.
 *
 * Each buffer has separate Y and UV planes, each imported from a distinct
 * DMA-BUF FD into CUDA as a device pointer.
 */
typedef struct _ExternalFdBuffer
{
    /* Plane FDs (owned by the external allocator — NOT closed by us) */
    int y_fd;
    int uv_fd;

    /* CUDA external memory handles */
    CUexternalMemory y_ext_mem;
    CUexternalMemory uv_ext_mem;

    /* CUDA device pointers mapped from the external memory */
    CUdeviceptr y_devptr;
    CUdeviceptr uv_devptr;

    /* Allocation sizes */
    gsize y_size;
    gsize uv_size;

    /* Row pitch (stride) in bytes — from Vulkan's image layout */
    guint y_stride;
    guint uv_stride;

    /* CUDA stream for async copy operations */
    CUstream cuda_stream;

    gboolean initialized;
} ExternalFdBuffer;

/**
 * ExternalFdPool - Ring buffer of imported external DMA-BUF buffers.
 */
typedef struct _ExternalFdPool
{
    ExternalFdBuffer buffers[EXTERNAL_FD_POOL_MAX_BUFFERS];
    guint count;
    guint current_index;

    /* Dimensions (for validation) */
    guint width;
    guint height;
    gboolean is_p010;

    gboolean initialized;
} ExternalFdPool;

/**
 * Import a DMA-BUF FD pair (Y + UV) into CUDA external memory.
 *
 * The FDs are NOT duplicated — they remain owned by the caller. The caller
 * must keep them open for the lifetime of this buffer.
 *
 * @param buf      Buffer to initialize
 * @param y_fd     DMA-BUF file descriptor for Y plane
 * @param y_size   Total allocation size of Y plane in bytes
 * @param y_stride Row pitch of Y plane in bytes
 * @param uv_fd    DMA-BUF file descriptor for UV plane
 * @param uv_size  Total allocation size of UV plane in bytes
 * @param uv_stride Row pitch of UV plane in bytes
 * @return TRUE on success
 */
gboolean external_fd_buffer_import(ExternalFdBuffer *buf,
                                   int y_fd, gsize y_size, guint y_stride,
                                   int uv_fd, gsize uv_size, guint uv_stride);

/**
 * Release CUDA external memory mappings.
 * Does NOT close the FDs (they're owned by the caller).
 */
void external_fd_buffer_release(ExternalFdBuffer *buf);

/**
 * Initialize an external FD pool.
 *
 * @param pool     Pool to initialize
 * @param width    Video width
 * @param height   Video height
 * @param is_p010  TRUE for P010 (10-bit), FALSE for NV12 (8-bit)
 * @return TRUE on success
 */
gboolean external_fd_pool_init(ExternalFdPool *pool,
                               guint width, guint height,
                               gboolean is_p010);

/**
 * Add a buffer pair to the pool by importing external FDs.
 *
 * @param pool      The pool
 * @param y_fd      Y plane DMA-BUF FD
 * @param y_size    Y plane allocation size
 * @param y_stride  Y plane row pitch
 * @param uv_fd     UV plane DMA-BUF FD
 * @param uv_size   UV plane allocation size
 * @param uv_stride UV plane row pitch
 * @return TRUE on success
 */
gboolean external_fd_pool_add(ExternalFdPool *pool,
                              int y_fd, gsize y_size, guint y_stride,
                              int uv_fd, gsize uv_size, guint uv_stride);

/**
 * Acquire the next buffer from the pool (round-robin).
 * Synchronizes on the buffer's CUDA stream first.
 *
 * @return Pointer to the buffer, or NULL if pool is empty/uninitialized
 */
ExternalFdBuffer *external_fd_pool_acquire(ExternalFdPool *pool);

/**
 * Clean up the pool. Releases all CUDA mappings.
 */
void external_fd_pool_cleanup(ExternalFdPool *pool);

G_END_DECLS

#endif /* __EXTERNAL_FD_POOL_H__ */
