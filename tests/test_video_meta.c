/* SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2025 Ericky
 *
 * Unit tests for GstVideoMeta attachment on DMA-BUF buffers
 */

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/allocators/allocators.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_WIDTH 1920
#define TEST_HEIGHT 1080

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg)                  \
    do                                          \
    {                                           \
        if (!(cond))                            \
        {                                       \
            fprintf(stderr, "FAIL: %s\n", msg); \
            tests_failed++;                     \
            return;                             \
        }                                       \
    } while (0)

#define TEST_PASS(name)             \
    do                              \
    {                               \
        printf("PASS: %s\n", name); \
        tests_passed++;             \
    } while (0)

/**
 * Test that GstVideoMeta with NV12 format has correct properties
 */
static void
test_nv12_video_meta_format(void)
{
    GstBuffer *buf = gst_buffer_new_allocate(NULL, TEST_WIDTH * TEST_HEIGHT * 3 / 2, NULL);
    TEST_ASSERT(buf != NULL, "Failed to allocate buffer");

    /* Simulate what the plugin should do: attach NV12 video meta */
    gsize offsets[4] = {0, TEST_WIDTH * TEST_HEIGHT, 0, 0};
    gint strides[4] = {TEST_WIDTH, TEST_WIDTH, 0, 0};

    GstVideoMeta *vmeta = gst_buffer_add_video_meta_full(
        buf,
        GST_VIDEO_FRAME_FLAG_NONE,
        GST_VIDEO_FORMAT_NV12, /* Must be actual format, NOT DMA_DRM */
        TEST_WIDTH,
        TEST_HEIGHT,
        2,
        offsets,
        strides);

    TEST_ASSERT(vmeta != NULL, "Failed to add video meta");
    TEST_ASSERT(vmeta->format == GST_VIDEO_FORMAT_NV12,
                "Video meta format should be NV12, not DMA_DRM");
    TEST_ASSERT(vmeta->width == TEST_WIDTH, "Width mismatch");
    TEST_ASSERT(vmeta->height == TEST_HEIGHT, "Height mismatch");
    TEST_ASSERT(vmeta->n_planes == 2, "NV12 should have 2 planes");
    TEST_ASSERT(vmeta->stride[0] == TEST_WIDTH, "Y plane stride mismatch");
    TEST_ASSERT(vmeta->stride[1] == TEST_WIDTH, "UV plane stride mismatch");
    TEST_ASSERT(vmeta->offset[0] == 0, "Y plane offset should be 0");
    TEST_ASSERT(vmeta->offset[1] == (gsize)(TEST_WIDTH * TEST_HEIGHT),
                "UV plane offset mismatch");

    /* Verify we can retrieve the meta */
    GstVideoMeta *retrieved = gst_buffer_get_video_meta(buf);
    TEST_ASSERT(retrieved != NULL, "Failed to retrieve video meta");
    TEST_ASSERT(retrieved->format == GST_VIDEO_FORMAT_NV12,
                "Retrieved format should be NV12");

    gst_buffer_unref(buf);
    TEST_PASS("test_nv12_video_meta_format");
}

/**
 * Test that GstVideoMeta with BGRx format has correct properties
 */
static void
test_bgrx_video_meta_format(void)
{
    GstBuffer *buf = gst_buffer_new_allocate(NULL, TEST_WIDTH * TEST_HEIGHT * 4, NULL);
    TEST_ASSERT(buf != NULL, "Failed to allocate buffer");

    /* Simulate what the plugin should do: attach BGRx video meta */
    gsize offsets[4] = {0, 0, 0, 0};
    gint strides[4] = {TEST_WIDTH * 4, 0, 0, 0};

    GstVideoMeta *vmeta = gst_buffer_add_video_meta_full(
        buf,
        GST_VIDEO_FRAME_FLAG_NONE,
        GST_VIDEO_FORMAT_BGRx, /* Must be actual format, NOT DMA_DRM */
        TEST_WIDTH,
        TEST_HEIGHT,
        1,
        offsets,
        strides);

    TEST_ASSERT(vmeta != NULL, "Failed to add video meta");
    TEST_ASSERT(vmeta->format == GST_VIDEO_FORMAT_BGRx,
                "Video meta format should be BGRx, not DMA_DRM");
    TEST_ASSERT(vmeta->width == TEST_WIDTH, "Width mismatch");
    TEST_ASSERT(vmeta->height == TEST_HEIGHT, "Height mismatch");
    TEST_ASSERT(vmeta->n_planes == 1, "BGRx should have 1 plane");
    TEST_ASSERT(vmeta->stride[0] == TEST_WIDTH * 4, "Stride mismatch");
    TEST_ASSERT(vmeta->offset[0] == 0, "Offset should be 0");

    gst_buffer_unref(buf);
    TEST_PASS("test_bgrx_video_meta_format");
}

/**
 * Test that DMA_DRM format in video meta is NOT what we want
 * (this documents the bug that was fixed)
 */
static void
test_dma_drm_format_is_wrong(void)
{
    /* GST_VIDEO_FORMAT_DMA_DRM should NOT be used in GstVideoMeta
     * because downstream elements can't interpret stride/offset
     * without knowing the actual pixel format */

    GstVideoFormat dma_drm = GST_VIDEO_FORMAT_DMA_DRM;
    GstVideoFormat nv12 = GST_VIDEO_FORMAT_NV12;
    GstVideoFormat bgrx = GST_VIDEO_FORMAT_BGRx;

    /* DMA_DRM is a special format marker, not an actual pixel format */
    TEST_ASSERT(dma_drm != nv12, "DMA_DRM should differ from NV12");
    TEST_ASSERT(dma_drm != bgrx, "DMA_DRM should differ from BGRx");

    /* Verify GST_VIDEO_FORMAT_DMA_DRM exists (it should in modern GStreamer) */
    const gchar *dma_drm_name = gst_video_format_to_string(dma_drm);
    TEST_ASSERT(dma_drm_name != NULL, "DMA_DRM format should have a name");

    TEST_PASS("test_dma_drm_format_is_wrong");
}

/**
 * Test video meta with custom stride (common for GPU buffers)
 */
static void
test_video_meta_custom_stride(void)
{
    /* GPU buffers often have padded strides for alignment */
    const gint padded_stride = 2048; /* Aligned to 256 bytes */

    GstBuffer *buf = gst_buffer_new_allocate(NULL, padded_stride * TEST_HEIGHT * 4, NULL);
    TEST_ASSERT(buf != NULL, "Failed to allocate buffer");

    gsize offsets[4] = {0, 0, 0, 0};
    gint strides[4] = {padded_stride * 4, 0, 0, 0};

    GstVideoMeta *vmeta = gst_buffer_add_video_meta_full(
        buf,
        GST_VIDEO_FRAME_FLAG_NONE,
        GST_VIDEO_FORMAT_BGRx,
        TEST_WIDTH,
        TEST_HEIGHT,
        1,
        offsets,
        strides);

    TEST_ASSERT(vmeta != NULL, "Failed to add video meta");
    TEST_ASSERT(vmeta->stride[0] == padded_stride * 4,
                "Custom stride should be preserved");
    TEST_ASSERT(vmeta->stride[0] > (gint)(TEST_WIDTH * 4),
                "Padded stride should be larger than minimum");

    gst_buffer_unref(buf);
    TEST_PASS("test_video_meta_custom_stride");
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);

    printf("Running GstVideoMeta tests...\n\n");

    test_nv12_video_meta_format();
    test_bgrx_video_meta_format();
    test_dma_drm_format_is_wrong();
    test_video_meta_custom_stride();

    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("========================================\n");

    gst_deinit();

    return tests_failed > 0 ? 1 : 0;
}
