/*
 * Buffer Transform Operations
 * Handles the actual buffer transform logic (NV12 passthrough, NV12→BGRx, BGRx copy)
 */

#include "buffer_transform.h"
#include "cuda_nv12_to_bgrx.h"

#define GST_USE_UNSTABLE_API
#include <gst/cuda/gstcuda.h>
#include <gst/allocators/allocators.h>
#include <gbm.h>
#include <unistd.h>
#include <string.h>
#include <cuda_runtime.h>

#define GST_CAT_DEFAULT gst_cuda_dmabuf_upload_debug
GST_DEBUG_CATEGORY_EXTERN(GST_CAT_DEFAULT);

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
        if (!cuda_egl_context_init(egl_ctx, "/dev/dri/renderD129"))
        {
            GST_ERROR("Failed to initialize CUDA-EGL context");
            return FALSE;
        }
    }

    /* Create dmabuf allocator if needed */
    if (!btx->dmabuf_allocator)
        btx->dmabuf_allocator = gst_dmabuf_allocator_new();

    return TRUE;
}

GstFlowReturn
buffer_transform_nv12_passthrough(BufferTransformContext *btx,
                                  PooledBufferPool *pool,
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

    /* Get input strides */
    GstVideoMeta *in_vmeta = gst_buffer_get_video_meta(inbuf);
    gint y_stride_in = in_vmeta ? in_vmeta->stride[0] : (gint)width;
    gint uv_stride_in = in_vmeta ? in_vmeta->stride[1] : (gint)width;
    gsize uv_offset_in = in_vmeta ? in_vmeta->offset[1] : (gsize)width * height;

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
        (size_t)width, (size_t)height,
        pool_buf->cuda_stream);

    if (cu_res != CUDA_SUCCESS)
    {
        GST_ERROR("Y plane copy failed: %d", cu_res);
        gst_buffer_unmap(inbuf, &in_map);
        return GST_FLOW_ERROR;
    }

    /* Async copy UV plane */
    cu_res = cuda_egl_copy_plane_async(
        in_base + uv_offset_in, (size_t)uv_stride_in,
        &pool_buf->cuda_frame, 1,
        (size_t)width, (size_t)(height / 2),
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

    /* Add video meta */
    gsize offsets[4] = {0, pool_buf->offsets[1], 0, 0};
    gint strides[4] = {(gint)pool_buf->strides[0], (gint)pool_buf->strides[1], 0, 0};
    gst_buffer_add_video_meta_full(*outbuf, GST_VIDEO_FRAME_FLAG_NONE,
                                   GST_VIDEO_FORMAT_DMA_DRM, width, height, 2, offsets, strides);

    /* Copy timestamps */
    GST_BUFFER_PTS(*outbuf) = GST_BUFFER_PTS(inbuf);
    GST_BUFFER_DTS(*outbuf) = GST_BUFFER_DTS(inbuf);
    GST_BUFFER_DURATION(*outbuf) = GST_BUFFER_DURATION(inbuf);

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

    /* Allocate single-use buffer for conversion */
    CudaEglBuffer conv_buf;
    if (!cuda_egl_buffer_alloc(btx->egl_ctx, &conv_buf, width, height,
                               GBM_FORMAT_XRGB8888, btx->negotiated_modifier))
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

    gsize offsets[4] = {0, 0, 0, 0};
    gint strides[4] = {(gint)cuda_pitch, 0, 0, 0};
    gst_buffer_add_video_meta_full(*outbuf, GST_VIDEO_FRAME_FLAG_NONE,
                                   GST_VIDEO_FORMAT_DMA_DRM, width, height, 1, offsets, strides);

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
