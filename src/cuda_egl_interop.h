/*
 * CUDA-EGL Interop Layer
 * Handles EGL display/context initialization and CUDA-EGL resource management
 */

#ifndef __CUDA_EGL_INTEROP_H__
#define __CUDA_EGL_INTEROP_H__

#include <glib.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <cuda.h>
#include <cudaEGL.h>
#include <gbm.h>

G_BEGIN_DECLS

/**
 * CudaEglContext - Manages EGL display and CUDA interop state
 */
typedef struct _CudaEglContext
{
    EGLDisplay egl_display;
    EGLContext egl_context;
    gboolean initialized;

    /* GBM device for buffer allocation */
    struct gbm_device *gbm;
    int drm_fd;
} CudaEglContext;

/**
 * Initialize CUDA-EGL interop context.
 * Opens the DRM render node and creates EGL display.
 *
 * @param ctx Context to initialize
 * @param drm_device Path to DRM render node (e.g., "/dev/dri/renderD129")
 * @return TRUE on success
 */
gboolean cuda_egl_context_init(CudaEglContext *ctx, const gchar *drm_device);

/**
 * Clean up CUDA-EGL context.
 */
void cuda_egl_context_cleanup(CudaEglContext *ctx);

/**
 * CudaEglBuffer - A GPU buffer accessible via both EGL and CUDA
 */
typedef struct _CudaEglBuffer
{
    struct gbm_bo *bo;
    int dmabuf_fd;
    EGLImageKHR egl_image;
    CUgraphicsResource cuda_resource;
    CUeglFrame cuda_frame;
    CUstream cuda_stream;

    /* Buffer properties */
    guint width;
    guint height;
    guint32 format; /* GBM/DRM format */
    guint64 modifier;

    /* Plane info (for multi-planar formats like NV12) */
    guint plane_count;
    guint strides[4];
    guint offsets[4];
    gsize size;

    gboolean in_use;
} CudaEglBuffer;

/**
 * Allocate a CUDA-EGL buffer with the specified format and modifier.
 *
 * @param ctx The CUDA-EGL context
 * @param buf Buffer structure to initialize
 * @param width Buffer width
 * @param height Buffer height
 * @param format GBM format (e.g., GBM_FORMAT_NV12, GBM_FORMAT_XRGB8888)
 * @param modifier DRM modifier for tiling (or DRM_FORMAT_MOD_LINEAR)
 * @return TRUE on success
 */
gboolean cuda_egl_buffer_alloc(CudaEglContext *ctx,
                               CudaEglBuffer *buf,
                               guint width,
                               guint height,
                               guint32 format,
                               guint64 modifier);

/**
 * Free a CUDA-EGL buffer and all associated resources.
 */
void cuda_egl_buffer_free(CudaEglContext *ctx, CudaEglBuffer *buf);

/**
 * Async copy from CUDA device memory to an EGL frame plane.
 *
 * @param src_dev Source device pointer
 * @param src_pitch Source pitch in bytes
 * @param dst Destination EGL frame
 * @param plane Plane index (0 for Y, 1 for UV in NV12)
 * @param width_bytes Width to copy in bytes
 * @param height_rows Number of rows to copy
 * @param stream CUDA stream for async operation
 * @return CUDA_SUCCESS on success
 */
CUresult cuda_egl_copy_plane_async(const void *src_dev,
                                   size_t src_pitch,
                                   CUeglFrame *dst,
                                   int plane,
                                   size_t width_bytes,
                                   size_t height_rows,
                                   CUstream stream);

/**
 * Synchronous copy (fallback when async not suitable).
 */
CUresult cuda_egl_copy_plane(const void *src_dev,
                             size_t src_pitch,
                             CUeglFrame *dst,
                             int plane,
                             size_t width_bytes,
                             size_t height_rows);

G_END_DECLS

#endif /* __CUDA_EGL_INTEROP_H__ */
