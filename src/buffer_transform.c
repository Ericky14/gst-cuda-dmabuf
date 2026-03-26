/* SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2025 Ericky
 *
 * Buffer Transform Operations
 * Handles the actual buffer transform logic (NV12 passthrough, NV12→BGRx, BGRx copy)
 */

#include "buffer_transform.h"
#include "cuda_nv12_to_bgrx.h"
#include "gstcudadmabufupload.h"

#define GST_USE_UNSTABLE_API
#include <gst/cuda/gstcuda.h>
#include <gst/allocators/allocators.h>
#include <gbm.h>
#include <unistd.h>
#include <string.h>
#include <cuda_runtime.h>
#include <dirent.h>
#include <limits.h>
#include <xf86drm.h>
#include <drm/drm_fourcc.h>
#include <fcntl.h>
#include <stdio.h>

/* Find NVIDIA render node path dynamically */
static const gchar *
find_nvidia_render_node_path(void)
{
    static gchar nvidia_path[PATH_MAX] = {0};

    if (nvidia_path[0] != '\0')
        return nvidia_path;

    DIR *dir = opendir("/dev/dri");
    if (!dir)
        return "/dev/dri/renderD128"; /* fallback */

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        if (strncmp(entry->d_name, "renderD", 7) != 0)
            continue;

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "/dev/dri/%s", entry->d_name);

        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;

        drmVersionPtr version = drmGetVersion(fd);
        if (version)
        {
            if (version->name && strcmp(version->name, "nvidia-drm") == 0)
            {
                drmFreeVersion(version);
                close(fd);
                strncpy(nvidia_path, path, sizeof(nvidia_path) - 1);
                closedir(dir);
                g_info("Found NVIDIA render node: %s", nvidia_path);
                return nvidia_path;
            }
            drmFreeVersion(version);
        }
        close(fd);
    }

    closedir(dir);
    return "/dev/dri/renderD128"; /* fallback */
}

gboolean
buffer_transform_context_init(BufferTransformContext *btx,
                              CudaEglContext *egl_ctx,
                              guint64 modifier)
{
    btx->egl_ctx = egl_ctx;
    btx->negotiated_modifier = modifier;

    /* Initialize EGL context if needed */
    if (!egl_ctx->initialized)
    {
        const gchar *drm_device = find_nvidia_render_node_path();
        if (!cuda_egl_context_init(egl_ctx, drm_device))
        {
            GST_ERROR("Failed to initialize CUDA-EGL context with %s", drm_device);
            return FALSE;
        }
    }

    /* Create dmabuf allocator if needed */
    if (!btx->dmabuf_allocator)
        btx->dmabuf_allocator = gst_dmabuf_allocator_new();

    return TRUE;
}

GstFlowReturn
buffer_transform_semi_planar_passthrough(BufferTransformContext *btx,
                                         PooledBufferPool *pool,
                                         GstBuffer *inbuf,
                                         GstBuffer **outbuf,
                                         const GstVideoInfo *info,
                                         gboolean is_p010)
{
    GstMemory *mem = gst_buffer_peek_memory(inbuf, 0);
    if (!gst_is_cuda_memory(mem))
    {
        GST_ERROR("Expected CUDA memory");
        return GST_FLOW_ERROR;
    }

    guint width = GST_VIDEO_INFO_WIDTH(info);
    guint height = GST_VIDEO_INFO_HEIGHT(info);

    /* P010 has 16-bit (2 byte) samples, NV12 has 8-bit (1 byte) */
    guint bytes_per_sample = is_p010 ? 2 : 1;
    guint width_bytes = width * bytes_per_sample;

    /* Get input strides */
    GstVideoMeta *in_vmeta = gst_buffer_get_video_meta(inbuf);
    gint y_stride_in = in_vmeta ? in_vmeta->stride[0] : (gint)width_bytes;
    gint uv_stride_in = in_vmeta ? in_vmeta->stride[1] : (gint)width_bytes;
    gsize uv_offset_in = in_vmeta ? in_vmeta->offset[1] : (gsize)width_bytes * height;

    /* Acquire next buffer from pool */
    CudaEglBuffer *pool_buf = pooled_buffer_pool_acquire(pool);
    if (!pool_buf)
    {
        GST_ERROR("Failed to acquire buffer from pool");
        return GST_FLOW_ERROR;
    }

    /* Map input CUDA buffer */
    GstMapInfo in_map;
    if (!gst_buffer_map(inbuf, &in_map, GST_MAP_READ | GST_MAP_CUDA))
    {
        GST_ERROR("Failed to map input buffer");
        return GST_FLOW_ERROR;
    }

    const uint8_t *in_base = (const uint8_t *)in_map.data;

    /* Async copy Y plane */
    CUresult cu_res = cuda_egl_copy_plane_async(
        in_base, (size_t)y_stride_in,
        &pool_buf->cuda_frame, 0,
        (size_t)width_bytes, (size_t)height,
        pool_buf->cuda_stream);

    if (cu_res != CUDA_SUCCESS)
    {
        GST_ERROR("Y plane copy failed: %d", cu_res);
        gst_buffer_unmap(inbuf, &in_map);
        return GST_FLOW_ERROR;
    }

    /* Async copy UV plane (interleaved U/V, half height) */
    cu_res = cuda_egl_copy_plane_async(
        in_base + uv_offset_in, (size_t)uv_stride_in,
        &pool_buf->cuda_frame, 1,
        (size_t)width_bytes, (size_t)(height / 2),
        pool_buf->cuda_stream);

    if (cu_res != CUDA_SUCCESS)
    {
        GST_ERROR("UV plane copy failed: %d", cu_res);
        gst_buffer_unmap(inbuf, &in_map);
        return GST_FLOW_ERROR;
    }

    gst_buffer_unmap(inbuf, &in_map);

    /* Sync before handing to compositor */
    cuStreamSynchronize(pool_buf->cuda_stream);

    /* Wrap DMABUF in GstBuffer */
    int fd_dup = dup(pool_buf->dmabuf_fd);
    if (fd_dup < 0)
    {
        GST_ERROR("Failed to dup fd");
        return GST_FLOW_ERROR;
    }

    GstMemory *dmabuf_mem = gst_dmabuf_allocator_alloc(
        btx->dmabuf_allocator, fd_dup, pool_buf->size);
    if (!dmabuf_mem)
    {
        close(fd_dup);
        return GST_FLOW_ERROR;
    }

    *outbuf = gst_buffer_new();
    gst_buffer_append_memory(*outbuf, dmabuf_mem);

    /* Add video meta with actual pixel format for proper stride/offset handling */
    GstVideoFormat vid_fmt = is_p010 ? GST_VIDEO_FORMAT_P010_10LE : GST_VIDEO_FORMAT_NV12;
    gsize offsets[4] = {0, pool_buf->offsets[1], 0, 0};
    gint strides[4] = {(gint)pool_buf->strides[0], (gint)pool_buf->strides[1], 0, 0};
    gst_buffer_add_video_meta_full(*outbuf, GST_VIDEO_FRAME_FLAG_NONE,
                                   vid_fmt, width, height, 2, offsets, strides);

    /* Copy timestamps */
    GST_BUFFER_PTS(*outbuf) = GST_BUFFER_PTS(inbuf);
    GST_BUFFER_DTS(*outbuf) = GST_BUFFER_DTS(inbuf);
    GST_BUFFER_DURATION(*outbuf) = GST_BUFFER_DURATION(inbuf);

    return GST_FLOW_OK;
}

GstFlowReturn
buffer_transform_cuda_export(BufferTransformContext *btx,
                             GstBuffer *inbuf,
                             GstBuffer **outbuf,
                             const GstVideoInfo *info)
{
    GstMemory *mem = gst_buffer_peek_memory(inbuf, 0);
    if (!gst_is_cuda_memory(mem))
    {
        GST_ERROR("Expected CUDA memory for direct export");
        return GST_FLOW_ERROR;
    }

    GstCudaMemory *cmem = GST_CUDA_MEMORY_CAST(mem);

    /* Verify this is MMAP-allocated (required for DMA-BUF export) */
    if (gst_cuda_memory_get_alloc_method(cmem) != GST_CUDA_MEMORY_ALLOC_MMAP)
    {
        GST_ERROR("CUDA memory is not MMAP-allocated, cannot export as DMA-BUF");
        return GST_FLOW_ERROR;
    }

    /* Sync CUDA operations before exporting */
    gst_cuda_memory_sync(cmem);

    /* Export CUDA memory as a POSIX file descriptor (DMA-BUF) */
    int fd = -1;
    if (!gst_cuda_memory_export(cmem, &fd))
    {
        GST_ERROR("Failed to export CUDA memory as DMA-BUF");
        return GST_FLOW_ERROR;
    }

    guint width = GST_VIDEO_INFO_WIDTH(info);
    guint height = GST_VIDEO_INFO_HEIGHT(info);

    /* Get stride/offset from video meta on the input buffer */
    GstVideoMeta *in_vmeta = gst_buffer_get_video_meta(inbuf);
    GstVideoFormat vid_fmt = GST_VIDEO_INFO_FORMAT(info);

    /* Calculate total size from video info or strides */
    gsize total_size;
    gint strides[4] = {0};
    gsize offsets[4] = {0};
    guint n_planes;

    if (in_vmeta)
    {
        n_planes = in_vmeta->n_planes;
        for (guint i = 0; i < n_planes && i < 4; i++)
        {
            strides[i] = in_vmeta->stride[i];
            offsets[i] = in_vmeta->offset[i];
        }
        /* Calculate total size: last plane offset + last plane data */
        if (n_planes == 2)
        {
            total_size = offsets[1] + (gsize)strides[1] * (height / 2);
        }
        else
        {
            total_size = (gsize)strides[0] * height;
        }
    }
    else
    {
        total_size = GST_VIDEO_INFO_SIZE(info);
        n_planes = GST_VIDEO_INFO_N_PLANES(info);
        for (guint i = 0; i < n_planes && i < 4; i++)
        {
            strides[i] = GST_VIDEO_INFO_PLANE_STRIDE(info, i);
            offsets[i] = GST_VIDEO_INFO_PLANE_OFFSET(info, i);
        }
    }

    /* Create DMA-BUF allocator if needed */
    if (!btx->dmabuf_allocator)
        btx->dmabuf_allocator = gst_dmabuf_allocator_new();

    GstMemory *dmabuf_mem = gst_dmabuf_allocator_alloc(btx->dmabuf_allocator, fd, total_size);
    if (!dmabuf_mem)
    {
        close(fd);
        GST_ERROR("Failed to wrap CUDA DMA-BUF fd in allocator");
        return GST_FLOW_ERROR;
    }

    *outbuf = gst_buffer_new();
    gst_buffer_append_memory(*outbuf, dmabuf_mem);

    /* Add video meta with correct format and plane info */
    gst_buffer_add_video_meta_full(*outbuf, GST_VIDEO_FRAME_FLAG_NONE,
                                   vid_fmt, width, height, n_planes, offsets, strides);

    /* Copy timestamps */
    GST_BUFFER_PTS(*outbuf) = GST_BUFFER_PTS(inbuf);
    GST_BUFFER_DTS(*outbuf) = GST_BUFFER_DTS(inbuf);
    GST_BUFFER_DURATION(*outbuf) = GST_BUFFER_DURATION(inbuf);

    GST_LOG("CUDA direct DMA-BUF export: fd=%d, %ux%u, size=%zu", fd, width, height, total_size);

    return GST_FLOW_OK;
}

GstFlowReturn
buffer_transform_nv12_to_bgrx(BufferTransformContext *btx,
                              GstBuffer *inbuf,
                              GstBuffer **outbuf,
                              const GstVideoInfo *info)
{
    GstMemory *mem = gst_buffer_peek_memory(inbuf, 0);
    if (!gst_is_cuda_memory(mem))
    {
        GST_ERROR("Expected CUDA memory");
        return GST_FLOW_ERROR;
    }

    guint width = GST_VIDEO_INFO_WIDTH(info);
    guint height = GST_VIDEO_INFO_HEIGHT(info);

    GstVideoMeta *in_vmeta = gst_buffer_get_video_meta(inbuf);
    gint y_stride = in_vmeta ? in_vmeta->stride[0] : (gint)width;
    gint uv_stride = in_vmeta ? in_vmeta->stride[1] : (gint)width;
    gsize uv_offset = in_vmeta ? in_vmeta->offset[1] : (gsize)width * height;

    /* Allocate single-use buffer for conversion
     * Force linear for XR24 since CUDA doesn't support tiled XR24 EGL interop */
    CudaEglBuffer conv_buf;
    if (!cuda_egl_buffer_alloc(btx->egl_ctx, &conv_buf, width, height,
                               GBM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, TRUE))
    {
        GST_ERROR("Failed to allocate conversion buffer");
        return GST_FLOW_ERROR;
    }

    /* Map input */
    GstMapInfo in_map;
    if (!gst_buffer_map(inbuf, &in_map, GST_MAP_READ | GST_MAP_CUDA))
    {
        cuda_egl_buffer_free(btx->egl_ctx, &conv_buf);
        GST_ERROR("Failed to map input");
        return GST_FLOW_ERROR;
    }

    CUdeviceptr cuda_out_ptr = (CUdeviceptr)conv_buf.cuda_frame.frame.pPitch[0];
    guint cuda_pitch = conv_buf.cuda_frame.pitch;

    /* Run NV12→BGRx kernel */
    int cuda_err = cuda_nv12_to_bgrx(
        in_map.data,
        (const uint8_t *)in_map.data + uv_offset,
        (void *)cuda_out_ptr,
        width, height,
        y_stride, uv_stride, cuda_pitch, NULL);

    cudaDeviceSynchronize();
    gst_buffer_unmap(inbuf, &in_map);

    /* Unregister CUDA but keep GBM/DMABUF */
    cuGraphicsUnregisterResource(conv_buf.cuda_resource);
    conv_buf.cuda_resource = NULL;

    /* Destroy CUDA stream - no longer needed after sync */
    if (conv_buf.cuda_stream)
    {
        cuStreamDestroy(conv_buf.cuda_stream);
        conv_buf.cuda_stream = NULL;
    }

    /* Destroy EGL image - no longer needed after CUDA unregister */
    cuda_egl_buffer_destroy_egl_image(btx->egl_ctx, &conv_buf);

    if (cuda_err != 0)
    {
        GST_ERROR("NV12→BGRx kernel failed: %d", cuda_err);
        cuda_egl_buffer_free(btx->egl_ctx, &conv_buf);
        return GST_FLOW_ERROR;
    }

    /* Create output buffer */
    GstMemory *dmabuf_mem = gst_dmabuf_allocator_alloc(
        btx->dmabuf_allocator, conv_buf.dmabuf_fd, conv_buf.size);
    conv_buf.dmabuf_fd = -1; /* Ownership transferred */

    if (!dmabuf_mem)
    {
        cuda_egl_buffer_free(btx->egl_ctx, &conv_buf);
        return GST_FLOW_ERROR;
    }

    *outbuf = gst_buffer_new();
    gst_buffer_append_memory(*outbuf, dmabuf_mem);

    /* Add video meta with actual BGRx format for proper stride/offset handling */
    gsize offsets[4] = {0, 0, 0, 0};
    gint strides[4] = {(gint)cuda_pitch, 0, 0, 0};
    gst_buffer_add_video_meta_full(*outbuf, GST_VIDEO_FRAME_FLAG_NONE,
                                   GST_VIDEO_FORMAT_BGRx, width, height, 1, offsets, strides);

    /* Copy timestamps */
    GST_BUFFER_PTS(*outbuf) = GST_BUFFER_PTS(inbuf);
    GST_BUFFER_DTS(*outbuf) = GST_BUFFER_DTS(inbuf);
    GST_BUFFER_DURATION(*outbuf) = GST_BUFFER_DURATION(inbuf);

    /* Store GBM BO with buffer for cleanup */
    gst_mini_object_set_qdata(GST_MINI_OBJECT(*outbuf),
                              g_quark_from_static_string("gbm-bo"),
                              conv_buf.bo, (GDestroyNotify)gbm_bo_destroy);
    conv_buf.bo = NULL;

    return GST_FLOW_OK;
}

GstFlowReturn
buffer_transform_bgrx_copy(GstBuffer *inbuf,
                           GstBuffer *outbuf,
                           const GstVideoInfo *info)
{
    guint width = GST_VIDEO_INFO_WIDTH(info);
    guint height = GST_VIDEO_INFO_HEIGHT(info);
    const guint row_bytes = width * 4;

    GstVideoFrame in_frame;
    if (!gst_video_frame_map(&in_frame, (GstVideoInfo *)info, inbuf, GST_MAP_READ))
    {
        GST_ERROR("Failed to map input");
        return GST_FLOW_ERROR;
    }

    GstMapInfo outmap;
    if (!gst_buffer_map(outbuf, &outmap, GST_MAP_WRITE))
    {
        gst_video_frame_unmap(&in_frame);
        return GST_FLOW_ERROR;
    }

    GstVideoMeta *vmeta = gst_buffer_get_video_meta(outbuf);
    gint dst_stride = vmeta ? vmeta->stride[0] : (gint)(width * 4);
    gint src_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&in_frame, 0);

    const guint8 *srcp = (const guint8 *)GST_VIDEO_FRAME_PLANE_DATA(&in_frame, 0);
    guint8 *dstp = (guint8 *)outmap.data;

    for (guint y = 0; y < height; y++)
    {
        memcpy(dstp, srcp, row_bytes);
        srcp += src_stride;
        dstp += dst_stride;
    }

    gst_buffer_unmap(outbuf, &outmap);
    gst_video_frame_unmap(&in_frame);

    return GST_FLOW_OK;
}
