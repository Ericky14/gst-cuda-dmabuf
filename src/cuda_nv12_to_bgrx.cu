/**
 * CUDA kernel for NV12 to BGRx color conversion
 * 
 * This runs the colorspace conversion on the GPU, keeping the zero-copy
 * pipeline entirely on GPU memory.
 * 
 * NV12 format:
 *   - Y plane: width x height, 1 byte per pixel
 *   - UV plane: (width/2) x (height/2), 2 bytes per pixel (U,V interleaved)
 * 
 * BGRx format:
 *   - 4 bytes per pixel (B, G, R, x)
 * 
 * Note: fix_glibc_cuda.h is pre-included via --pre-include to work around
 * glibc 2.41 sinpi/cospi noexcept conflicts.
 */

#include <cuda_runtime.h>

/**
 * NV12 to BGRx conversion kernel
 * 
 * Each thread handles one pixel in the output BGRx image.
 * Uses BT.709 coefficients for HD content.
 */
__global__ void nv12_to_bgrx_kernel(
    const unsigned char* __restrict__ y_plane,
    const unsigned char* __restrict__ uv_plane,
    unsigned char* __restrict__ bgrx_out,
    int width,
    int height,
    int y_stride,
    int uv_stride,
    int out_stride)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    
    if (x >= width || y >= height)
        return;
    
    /* Sample Y at full resolution */
    int y_idx = y * y_stride + x;
    float Y = (float)y_plane[y_idx];
    
    /* Sample UV at half resolution (NV12 is 4:2:0) */
    int uv_x = x / 2;
    int uv_y = y / 2;
    int uv_idx = uv_y * uv_stride + uv_x * 2;
    
    float U = (float)uv_plane[uv_idx] - 128.0f;
    float V = (float)uv_plane[uv_idx + 1] - 128.0f;
    
    /* BT.709 YUV to RGB conversion (for HD content)
     * R = Y + 1.5748 * V
     * G = Y - 0.1873 * U - 0.4681 * V
     * B = Y + 1.8556 * U
     */
    float R = Y + 1.5748f * V;
    float G = Y - 0.1873f * U - 0.4681f * V;
    float B = Y + 1.8556f * U;
    
    /* Clamp to 0-255 using ternary (no fminf/fmaxf to avoid headers) */
    if (R < 0.0f) R = 0.0f; else if (R > 255.0f) R = 255.0f;
    if (G < 0.0f) G = 0.0f; else if (G > 255.0f) G = 255.0f;
    if (B < 0.0f) B = 0.0f; else if (B > 255.0f) B = 255.0f;
    
    /* Write BGRx output (4 bytes per pixel) */
    int out_idx = y * out_stride + x * 4;
    bgrx_out[out_idx + 0] = (unsigned char)B;
    bgrx_out[out_idx + 1] = (unsigned char)G;
    bgrx_out[out_idx + 2] = (unsigned char)R;
    bgrx_out[out_idx + 3] = 255;  /* x = 0xFF */
}

extern "C" {

/**
 * Host function to launch the NV12 to BGRx conversion
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
    void* stream)
{
    /* Use 16x16 thread blocks */
    dim3 block(16, 16);
    dim3 grid((width + block.x - 1) / block.x,
              (height + block.y - 1) / block.y);
    
    nv12_to_bgrx_kernel<<<grid, block, 0, (cudaStream_t)stream>>>(
        (const unsigned char*)y_plane,
        (const unsigned char*)uv_plane,
        (unsigned char*)bgrx_out,
        width, height,
        y_stride, uv_stride, out_stride);
    
    return (int)cudaGetLastError();
}

} /* extern "C" */
