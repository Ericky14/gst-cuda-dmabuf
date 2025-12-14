/* SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2025 Ericky
 *
 * CUDA-EGL Interop Layer
 * Handles EGL display/context initialization and CUDA-EGL resource management
 */

#include "cuda_egl_interop.h"

#include <drm/drm_fourcc.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

/* EGL extension function pointers */
static PFNEGLGETPLATFORMDISPLAYEXTPROC _eglGetPlatformDisplayEXT = NULL;
static PFNEGLCREATEIMAGEKHRPROC _eglCreateImageKHR = NULL;
static PFNEGLDESTROYIMAGEKHRPROC _eglDestroyImageKHR = NULL;

static gboolean
load_egl_extensions(void)
{
    static gboolean loaded = FALSE;
    if (loaded)
        return TRUE;

    _eglGetPlatformDisplayEXT =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    _eglCreateImageKHR =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    _eglDestroyImageKHR =
        (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");

    loaded = TRUE;
    return (_eglCreateImageKHR != NULL && _eglDestroyImageKHR != NULL);
}

gboolean
cuda_egl_context_init(CudaEglContext *ctx, const gchar *drm_device)
{
    g_return_val_if_fail(ctx != NULL, FALSE);
    g_return_val_if_fail(drm_device != NULL, FALSE);

    memset(ctx, 0, sizeof(CudaEglContext));
    ctx->drm_fd = -1;
    ctx->egl_display = EGL_NO_DISPLAY;
    ctx->egl_context = EGL_NO_CONTEXT;

    if (!load_egl_extensions())
    {
        g_warning("Failed to load required EGL extensions");
        return FALSE;
    }

    /* Open DRM render node */
    ctx->drm_fd = open(drm_device, O_RDWR | O_CLOEXEC);
    if (ctx->drm_fd < 0)
    {
        g_warning("Failed to open DRM device: %s", drm_device);
        return FALSE;
    }

    /* Create GBM device */
    ctx->gbm = gbm_create_device(ctx->drm_fd);
    if (!ctx->gbm)
    {
        g_warning("Failed to create GBM device");
        close(ctx->drm_fd);
        ctx->drm_fd = -1;
        return FALSE;
    }

    /* Get EGL display from GBM */
    if (_eglGetPlatformDisplayEXT)
    {
        ctx->egl_display = _eglGetPlatformDisplayEXT(
            EGL_PLATFORM_GBM_MESA, ctx->gbm, NULL);
    }
    else
    {
        ctx->egl_display = eglGetDisplay((EGLNativeDisplayType)ctx->gbm);
    }

    if (ctx->egl_display == EGL_NO_DISPLAY)
    {
        g_warning("Failed to get EGL display: 0x%x", eglGetError());
        gbm_device_destroy(ctx->gbm);
        ctx->gbm = NULL;
        close(ctx->drm_fd);
        ctx->drm_fd = -1;
        return FALSE;
    }

    /* Initialize EGL */
    EGLint major, minor;
    if (!eglInitialize(ctx->egl_display, &major, &minor))
    {
        g_warning("Failed to initialize EGL: 0x%x", eglGetError());
        ctx->egl_display = EGL_NO_DISPLAY;
        gbm_device_destroy(ctx->gbm);
        ctx->gbm = NULL;
        close(ctx->drm_fd);
        ctx->drm_fd = -1;
        return FALSE;
    }

    /* Initialize CUDA driver API */
    CUresult cu_res = cuInit(0);
    if (cu_res != CUDA_SUCCESS)
    {
        g_warning("cuInit failed: %d", cu_res);
        eglTerminate(ctx->egl_display);
        ctx->egl_display = EGL_NO_DISPLAY;
        gbm_device_destroy(ctx->gbm);
        ctx->gbm = NULL;
        close(ctx->drm_fd);
        ctx->drm_fd = -1;
        return FALSE;
    }

    ctx->initialized = TRUE;
    g_debug("CUDA-EGL context initialized (EGL %d.%d)", major, minor);
    return TRUE;
}

void cuda_egl_context_cleanup(CudaEglContext *ctx)
{
    if (!ctx)
        return;

    if (ctx->egl_context != EGL_NO_CONTEXT)
    {
        eglDestroyContext(ctx->egl_display, ctx->egl_context);
        ctx->egl_context = EGL_NO_CONTEXT;
    }

    if (ctx->egl_display != EGL_NO_DISPLAY)
    {
        eglTerminate(ctx->egl_display);
        ctx->egl_display = EGL_NO_DISPLAY;
    }

    if (ctx->gbm)
    {
        gbm_device_destroy(ctx->gbm);
        ctx->gbm = NULL;
    }

    if (ctx->drm_fd >= 0)
    {
        close(ctx->drm_fd);
        ctx->drm_fd = -1;
    }

    ctx->initialized = FALSE;
}

gboolean
cuda_egl_buffer_alloc(CudaEglContext *ctx,
                      CudaEglBuffer *buf,
                      guint width,
                      guint height,
                      guint32 format,
                      guint64 modifier)
{
    g_return_val_if_fail(ctx != NULL && ctx->initialized, FALSE);
    g_return_val_if_fail(buf != NULL, FALSE);

    memset(buf, 0, sizeof(CudaEglBuffer));
    buf->dmabuf_fd = -1;
    buf->width = width;
    buf->height = height;
    buf->format = format;

    /* Try to create with requested modifier */
    if (modifier != DRM_FORMAT_MOD_INVALID && modifier != DRM_FORMAT_MOD_LINEAR)
    {
        uint64_t mods[] = {modifier};
        buf->bo = gbm_bo_create_with_modifiers(ctx->gbm, width, height, format, mods, 1);
    }

    /* Fallback to LINEAR */
    if (!buf->bo)
    {
        buf->bo = gbm_bo_create(ctx->gbm, width, height, format,
                                GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
        modifier = DRM_FORMAT_MOD_LINEAR;
    }

    if (!buf->bo)
    {
        g_warning("Failed to create GBM buffer %ux%u format=0x%x", width, height, format);
        return FALSE;
    }

    buf->modifier = gbm_bo_get_modifier(buf->bo);
    buf->dmabuf_fd = gbm_bo_get_fd(buf->bo);
    if (buf->dmabuf_fd < 0)
    {
        g_warning("Failed to get dmabuf fd");
        gbm_bo_destroy(buf->bo);
        buf->bo = NULL;
        return FALSE;
    }

    /* Get plane info */
    buf->plane_count = gbm_bo_get_plane_count(buf->bo);
    for (guint i = 0; i < buf->plane_count && i < 4; i++)
    {
        buf->strides[i] = gbm_bo_get_stride_for_plane(buf->bo, i);
        buf->offsets[i] = gbm_bo_get_offset(buf->bo, i);
    }

    /* Calculate total size */
    if (format == GBM_FORMAT_NV12)
    {
        buf->size = buf->offsets[1] + (gsize)buf->strides[1] * (height / 2);
    }
    else
    {
        buf->size = (gsize)buf->strides[0] * height;
    }

    /* Create EGLImage */
    EGLint attribs[32];
    int ai = 0;

    attribs[ai++] = EGL_WIDTH;
    attribs[ai++] = (EGLint)width;
    attribs[ai++] = EGL_HEIGHT;
    attribs[ai++] = (EGLint)height;
    attribs[ai++] = EGL_LINUX_DRM_FOURCC_EXT;
    attribs[ai++] = (EGLint)format;
    attribs[ai++] = EGL_DMA_BUF_PLANE0_FD_EXT;
    attribs[ai++] = buf->dmabuf_fd;
    attribs[ai++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
    attribs[ai++] = 0;
    attribs[ai++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
    attribs[ai++] = (EGLint)buf->strides[0];
    attribs[ai++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
    attribs[ai++] = (EGLint)(buf->modifier & 0xffffffff);
    attribs[ai++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
    attribs[ai++] = (EGLint)(buf->modifier >> 32);

    /* Add plane 1 for NV12 */
    if (format == GBM_FORMAT_NV12 && buf->plane_count >= 2)
    {
        attribs[ai++] = EGL_DMA_BUF_PLANE1_FD_EXT;
        attribs[ai++] = buf->dmabuf_fd;
        attribs[ai++] = EGL_DMA_BUF_PLANE1_OFFSET_EXT;
        attribs[ai++] = (EGLint)buf->offsets[1];
        attribs[ai++] = EGL_DMA_BUF_PLANE1_PITCH_EXT;
        attribs[ai++] = (EGLint)buf->strides[1];
        attribs[ai++] = EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT;
        attribs[ai++] = (EGLint)(buf->modifier & 0xffffffff);
        attribs[ai++] = EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT;
        attribs[ai++] = (EGLint)(buf->modifier >> 32);
    }

    attribs[ai++] = EGL_NONE;

    buf->egl_image = _eglCreateImageKHR(ctx->egl_display, EGL_NO_CONTEXT,
                                        EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
    if (buf->egl_image == EGL_NO_IMAGE_KHR)
    {
        g_warning("Failed to create EGLImage: 0x%x", eglGetError());
        close(buf->dmabuf_fd);
        buf->dmabuf_fd = -1;
        gbm_bo_destroy(buf->bo);
        buf->bo = NULL;
        return FALSE;
    }

    /* Register with CUDA */
    CUresult cu_res = cuGraphicsEGLRegisterImage(&buf->cuda_resource, buf->egl_image, 0);
    if (cu_res != CUDA_SUCCESS)
    {
        g_warning("cuGraphicsEGLRegisterImage failed: %d", cu_res);
        _eglDestroyImageKHR(ctx->egl_display, buf->egl_image);
        buf->egl_image = EGL_NO_IMAGE_KHR;
        close(buf->dmabuf_fd);
        buf->dmabuf_fd = -1;
        gbm_bo_destroy(buf->bo);
        buf->bo = NULL;
        return FALSE;
    }

    /* Get mapped EGL frame */
    cu_res = cuGraphicsResourceGetMappedEglFrame(&buf->cuda_frame, buf->cuda_resource, 0, 0);
    if (cu_res != CUDA_SUCCESS)
    {
        g_warning("cuGraphicsResourceGetMappedEglFrame failed: %d", cu_res);
        cuGraphicsUnregisterResource(buf->cuda_resource);
        buf->cuda_resource = NULL;
        _eglDestroyImageKHR(ctx->egl_display, buf->egl_image);
        buf->egl_image = EGL_NO_IMAGE_KHR;
        close(buf->dmabuf_fd);
        buf->dmabuf_fd = -1;
        gbm_bo_destroy(buf->bo);
        buf->bo = NULL;
        return FALSE;
    }

    /* Create CUDA stream for async operations */
    cu_res = cuStreamCreate(&buf->cuda_stream, CU_STREAM_NON_BLOCKING);
    if (cu_res != CUDA_SUCCESS)
    {
        g_warning("cuStreamCreate failed: %d", cu_res);
        cuGraphicsUnregisterResource(buf->cuda_resource);
        buf->cuda_resource = NULL;
        _eglDestroyImageKHR(ctx->egl_display, buf->egl_image);
        buf->egl_image = EGL_NO_IMAGE_KHR;
        close(buf->dmabuf_fd);
        buf->dmabuf_fd = -1;
        gbm_bo_destroy(buf->bo);
        buf->bo = NULL;
        return FALSE;
    }

    buf->in_use = FALSE;
    return TRUE;
}

void cuda_egl_buffer_free(CudaEglContext *ctx, CudaEglBuffer *buf)
{
    if (!buf)
        return;

    if (buf->cuda_stream)
    {
        cuStreamSynchronize(buf->cuda_stream);
        cuStreamDestroy(buf->cuda_stream);
        buf->cuda_stream = NULL;
    }

    if (buf->cuda_resource)
    {
        cuGraphicsUnregisterResource(buf->cuda_resource);
        buf->cuda_resource = NULL;
    }

    if (buf->egl_image != EGL_NO_IMAGE_KHR && ctx && ctx->egl_display != EGL_NO_DISPLAY)
    {
        _eglDestroyImageKHR(ctx->egl_display, buf->egl_image);
        buf->egl_image = EGL_NO_IMAGE_KHR;
    }

    if (buf->dmabuf_fd >= 0)
    {
        close(buf->dmabuf_fd);
        buf->dmabuf_fd = -1;
    }

    if (buf->bo)
    {
        gbm_bo_destroy(buf->bo);
        buf->bo = NULL;
    }
}

void cuda_egl_buffer_destroy_egl_image(CudaEglContext *ctx, CudaEglBuffer *buf)
{
    if (!buf)
        return;

    if (buf->egl_image != EGL_NO_IMAGE_KHR && ctx && ctx->egl_display != EGL_NO_DISPLAY)
    {
        _eglDestroyImageKHR(ctx->egl_display, buf->egl_image);
        buf->egl_image = EGL_NO_IMAGE_KHR;
    }
}

CUresult
cuda_egl_copy_plane_async(const void *src_dev,
                          size_t src_pitch,
                          CUeglFrame *dst,
                          int plane,
                          size_t width_bytes,
                          size_t height_rows,
                          CUstream stream)
{
    CUDA_MEMCPY2D c;
    memset(&c, 0, sizeof(c));
    c.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    c.srcDevice = (CUdeviceptr)src_dev;
    c.srcPitch = src_pitch;

    if (dst->frameType == CU_EGL_FRAME_TYPE_PITCH)
    {
        c.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        c.dstDevice = (CUdeviceptr)dst->frame.pPitch[plane];
        c.dstPitch = dst->pitch;
        c.WidthInBytes = width_bytes;
        c.Height = height_rows;
        return cuMemcpy2DAsync(&c, stream);
    }
    else if (dst->frameType == CU_EGL_FRAME_TYPE_ARRAY)
    {
        c.dstMemoryType = CU_MEMORYTYPE_ARRAY;
        c.dstArray = dst->frame.pArray[plane];
        c.WidthInBytes = width_bytes;
        c.Height = height_rows;
        return cuMemcpy2DAsync(&c, stream);
    }

    return CUDA_ERROR_INVALID_VALUE;
}

CUresult
cuda_egl_copy_plane(const void *src_dev,
                    size_t src_pitch,
                    CUeglFrame *dst,
                    int plane,
                    size_t width_bytes,
                    size_t height_rows)
{
    CUDA_MEMCPY2D c;
    memset(&c, 0, sizeof(c));
    c.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    c.srcDevice = (CUdeviceptr)src_dev;
    c.srcPitch = src_pitch;

    if (dst->frameType == CU_EGL_FRAME_TYPE_PITCH)
    {
        c.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        c.dstDevice = (CUdeviceptr)dst->frame.pPitch[plane];
        c.dstPitch = dst->pitch;
        c.WidthInBytes = width_bytes;
        c.Height = height_rows;
        return cuMemcpy2D(&c);
    }
    else if (dst->frameType == CU_EGL_FRAME_TYPE_ARRAY)
    {
        c.dstMemoryType = CU_MEMORYTYPE_ARRAY;
        c.dstArray = dst->frame.pArray[plane];
        c.WidthInBytes = width_bytes;
        c.Height = height_rows;
        return cuMemcpy2D(&c);
    }

    return CUDA_ERROR_INVALID_VALUE;
}
