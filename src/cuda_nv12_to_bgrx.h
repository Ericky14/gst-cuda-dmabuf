/**
 * CUDA NV12 to BGRx conversion - C header
 */

#ifndef CUDA_NV12_TO_BGRX_H
#define CUDA_NV12_TO_BGRX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Convert NV12 image to BGRx on the GPU
 * 
 * @param y_plane     Pointer to Y plane in device memory
 * @param uv_plane    Pointer to UV plane in device memory  
 * @param bgrx_out    Pointer to output BGRx buffer in device memory
 * @param width       Image width
 * @param height      Image height
 * @param y_stride    Stride of Y plane in bytes
 * @param uv_stride   Stride of UV plane in bytes
 * @param out_stride  Stride of output BGRx buffer in bytes
 * @param stream      CUDA stream to use (NULL/0 for default)
 * 
 * @return 0 (cudaSuccess) on success, CUDA error code otherwise
 */
int cuda_nv12_to_bgrx(
    const void* y_plane,
    const void* uv_plane,
    void* bgrx_out,
    int width,
    int height,
    int y_stride,
    int uv_stride,
    int out_stride,
    void* stream);

#ifdef __cplusplus
}
#endif

#endif /* CUDA_NV12_TO_BGRX_H */
