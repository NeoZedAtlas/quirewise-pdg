#include "pdg_internal.h"
#include "../../core/byte_utils.h"
#include "../image/image_backend.h"

#include <stdlib.h>
#include <string.h>

#include <limits.h>

#include <quirewise/image.h>



static bool is_group4_type(uint8_t type)
{
    return type == 0x02 || type == 0x03 || type == 0x10 || (type >= 0x11 && type <= 0x1c);
}

static bool is_group4_payload(const PdgBuffer *payload)
{
    /* Payload signatures identify native containers such as the PDF member
     * type 0x1a; do not reinterpret those bytes as Group4 based on the header. */
    if (!payload || payload->kind != PDG_OUTPUT_RAW) {
        return false;
    }
    if (is_group4_type(payload->header.type)) {
        return true;
    }
    return payload->header.type == 0x00 && payload->header.color_depth == 1 && payload->kind == PDG_OUTPUT_RAW;
}

static int render_group4_payload(PdgContext *ctx, const PdgBuffer *payload, PdgImage *out)
{
    int rc = qw_pdg_decode_g4_to_gray8_ctx(ctx,
                                        payload->data,
                                        payload->size,
                                        payload->header.width,
                                        payload->header.height,
                                        out);
    if (rc == PDG_OK) {
        out->header = payload->header;
    }
    return rc;
}

static int render_bmp_to_image(const uint8_t *data, size_t size, const PdgHeader *header, PdgImage *out)
{
    QwPageImage page = {0};
    size_t stride;
    size_t expected_size;
    int rc;

    if (!out) return PDG_ERR_INVALID_ARGUMENT;
    rc = qw_bmp_decode_page(data, size, &page);
    if (rc != PDG_OK) return rc;
    if (page.kind != QW_PAGE_IMAGE_RGB24 || page.width > UINT32_MAX / 3u ||
        !qw_checked_size_mul((size_t)page.width, 3u, &stride) ||
        !qw_checked_size_mul(stride, (size_t)page.height, &expected_size) ||
        page.size < expected_size) {
        qw_image_release_page(&page);
        return PDG_ERR_FORMAT;
    }

    memset(out, 0, sizeof(*out));
    out->pixels = (uint8_t *)page.data;
    out->size = page.size;
    out->width = page.width;
    out->height = page.height;
    out->stride = (uint32_t)stride;
    out->format = PDG_PIXEL_RGB24;
    if (header) out->header = *header;
    page.data = NULL;
    qw_image_release_page(&page);
    return PDG_OK;
}

static int render_png_to_rgb24(const uint8_t *data,
                               size_t size,
                               const PdgHeader *header,
                               PdgImage *out)
{
    QwPageImage page = {0};
    uint8_t *pixels = NULL;
    size_t pixel_count;
    size_t stride;
    size_t image_size;
    int rc;

    rc = qw_png_decode_page(data, size, &page);
    if (rc != PDG_OK) return rc;
    if (!page.width || !page.height || page.width > UINT32_MAX / 3u ||
        !qw_checked_size_mul((size_t)page.width, (size_t)page.height,
                             &pixel_count) ||
        !qw_checked_size_mul((size_t)page.width, 3u, &stride) ||
        !qw_checked_size_mul(stride, (size_t)page.height, &image_size)) {
        rc = PDG_ERR_FORMAT;
        goto cleanup;
    }
    if (page.kind == QW_PAGE_IMAGE_RGB24) {
        if (page.size < image_size) {
            rc = PDG_ERR_FORMAT;
            goto cleanup;
        }
        pixels = (uint8_t *)page.data;
        page.data = NULL;
    } else if (page.kind == QW_PAGE_IMAGE_GRAY8) {
        if (page.size < pixel_count) {
            rc = PDG_ERR_FORMAT;
            goto cleanup;
        }
        pixels = (uint8_t *)malloc(image_size ? image_size : 1u);
        if (!pixels) {
            rc = PDG_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
        for (size_t i = 0u; i < pixel_count; ++i) {
            uint8_t gray = page.data[i];
            pixels[i * 3u] = gray;
            pixels[i * 3u + 1u] = gray;
            pixels[i * 3u + 2u] = gray;
        }
    } else {
        rc = PDG_ERR_FORMAT;
        goto cleanup;
    }

    memset(out, 0, sizeof(*out));
    out->pixels = pixels;
    out->size = image_size;
    out->width = page.width;
    out->height = page.height;
    out->stride = (uint32_t)stride;
    out->format = PDG_PIXEL_RGB24;
    if (header) out->header = *header;
    pixels = NULL;
    rc = PDG_OK;

cleanup:
    qw_image_release_page(&page);
    free(pixels);
    return rc;
}

static int render_encoded_image_to_rgb24(const uint8_t *data, size_t size, const PdgHeader *header, PdgImage *out)
{
    uint32_t parsed_width;
    uint32_t parsed_height;
    QwImage image;
    int width;
    int height;
    int status;
    int rc;
    size_t stride;
    size_t image_size;
    size_t i;
    uint8_t *pixels;

    if (data && out && qw_png_probe(data, size)) {
        return render_png_to_rgb24(data, size, header, out);
    }
    if (!data || !out || size > INT_MAX) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    rc = qw_jpeg_parse(data, size, &parsed_width, &parsed_height);
    if (rc != PDG_OK) return rc;
    if (!qw_pdg_image_dimensions_within_limits(parsed_width, parsed_height)) {
        return PDG_ERR_RESOURCE_LIMIT;
    }

    status = qw_image_decode(data, size, &image);
    if (status != QW_IMAGE_OK) return PDG_ERR_DECODE;
    width = (int)image.width;
    height = (int)image.height;
    if (width <= 0 || height <= 0) {
        qw_image_free(&image);
        return PDG_ERR_DECODE;
    }
    if ((uint32_t)width != parsed_width ||
        (uint32_t)height != parsed_height) {
        qw_image_free(&image);
        return PDG_ERR_FORMAT;
    }
    stride = (size_t)width * 3u;
    image_size = stride * (size_t)height;
    if (stride == 0 || image_size / stride != (size_t)height) {
        qw_image_free(&image);
        return PDG_ERR_FORMAT;
    }
    pixels = (uint8_t *)malloc(image_size);
    if (!pixels) {
        qw_image_free(&image);
        return PDG_ERR_OUT_OF_MEMORY;
    }
    if (image.format == QW_IMAGE_PIXEL_RGB24) {
        memcpy(pixels, image.pixels, image_size);
    } else {
        for (i = 0; i < (size_t)width * (size_t)height; ++i) {
            uint8_t gray = image.pixels[i];
            pixels[i * 3u + 0u] = gray;
            pixels[i * 3u + 1u] = gray;
            pixels[i * 3u + 2u] = gray;
        }
    }
    qw_image_free(&image);

    memset(out, 0, sizeof(*out));
    out->pixels = pixels;
    out->size = image_size;
    out->width = (uint32_t)width;
    out->height = (uint32_t)height;
    out->stride = (uint32_t)stride;
    out->format = PDG_PIXEL_RGB24;
    if (header) {
        out->header = *header;
    }
    return PDG_OK;
}

int pdg_render_jp2_payload(const uint8_t *data,
                           size_t size,
                           const PdgHeader *header,
                           PdgImage *out)
{
    (void)header;
    if (!data || !out) return PDG_ERR_INVALID_ARGUMENT;
    (void)size;
    memset(out, 0, sizeof(*out));
    return PDG_ERR_EXTERNAL_DECODER_REQUIRED;
}

int pdg_render_decoded_payload_ctx(PdgContext *ctx,
                                   const PdgBuffer *payload,
                                   PdgImage *out)
{
    if (!payload || !out || (!payload->data && payload->size != 0)) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));

    int rc;
    if (is_group4_payload(payload)) {
        rc = render_group4_payload(ctx, payload, out);
    } else if (payload->kind == PDG_OUTPUT_BMP) {
        rc = render_bmp_to_image(payload->data, payload->size, &payload->header, out);
    } else if (payload->kind == PDG_OUTPUT_JPEG || payload->kind == PDG_OUTPUT_PNG) {
        rc = render_encoded_image_to_rgb24(payload->data, payload->size, &payload->header, out);
    } else if (payload->kind == PDG_OUTPUT_DJVU) {
        rc = pdg_render_djvu_payload(payload->data, payload->size, &payload->header, out);
    } else if (payload->kind == PDG_OUTPUT_TIFF) {
        rc = pdg_render_tiff_payload(payload->data, payload->size, &payload->header, out);
    } else if (payload->kind == PDG_OUTPUT_JP2) {
        rc = pdg_render_jp2_payload(payload->data, payload->size, &payload->header, out);
    } else if (payload->kind == PDG_OUTPUT_PDF) {
        rc = PDG_ERR_EXTERNAL_DECODER_REQUIRED;
    } else if (pdg_output_requires_external_decoder(payload->kind)) {
        rc = PDG_ERR_EXTERNAL_DECODER_REQUIRED;
    } else {
        rc = PDG_ERR_UNSUPPORTED;
    }

    if (rc != PDG_OK) {
        memset(out, 0, sizeof(*out));
    }
    return rc;
}

PDG_API int pdg_render_memory_ctx(PdgContext *ctx, const void *data, size_t size, PdgImage *out)
{
    if ((!data && size != 0) || !out) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));

    PdgBuffer payload;
    int rc = pdg_decrypt_memory_ctx(ctx, data, size, &payload);
    if (rc != PDG_OK) {
        return rc;
    }
    rc = pdg_render_decoded_payload_ctx(ctx, &payload, out);
    pdg_buffer_free(&payload);
    return rc;
}

PDG_API int pdg_render_memory(const void *data, size_t size, PdgImage *out)
{
    return pdg_render_memory_ctx(NULL, data, size, out);
}

PDG_API void pdg_image_free(PdgImage *image)
{
    if (!image) {
        return;
    }
    free(image->pixels);
    memset(image, 0, sizeof(*image));
}
