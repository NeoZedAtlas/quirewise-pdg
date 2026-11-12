#include "pdz_pdf.h"
#include "pdg.h"
#include "pdg_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct PdzPdfBytes {
    uint8_t *data;
    size_t size;
    size_t capacity;
    /* Optional staging sink: when set, the buffer is drained every
     * PDZ_PDF_STAGE_BYTES so a streaming writer holds one object at a time
     * instead of the whole document. */
    int (*flush)(void *context, const void *data, size_t size);
    void *flush_context;
    uint64_t flushed;
} PdzPdfBytes;

#define PDZ_PDF_STAGE_BYTES ((size_t)65536u)

static uint64_t bytes_total(const PdzPdfBytes *bytes)
{
    return bytes ? bytes->flushed + (uint64_t)bytes->size : 0u;
}

static int bytes_reserve(PdzPdfBytes *bytes, size_t extra)
{
    size_t required;
    size_t capacity;
    uint8_t *grown;

    if (!bytes || extra > SIZE_MAX - bytes->size) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    required = bytes->size + extra;
    if (required <= bytes->capacity) {
        return PDG_OK;
    }
    capacity = bytes->capacity ? bytes->capacity : 4096u;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2u) {
            capacity = required;
            break;
        }
        capacity *= 2u;
    }
    grown = (uint8_t *)realloc(bytes->data, capacity);
    if (!grown) {
        return PDG_ERR_OUT_OF_MEMORY;
    }
    bytes->data = grown;
    bytes->capacity = capacity;
    return PDG_OK;
}

static int bytes_flush_all(PdzPdfBytes *bytes)
{
    int rc;
    if (!bytes || !bytes->flush || bytes->size == 0u) {
        return PDG_OK;
    }
    rc = bytes->flush(bytes->flush_context, bytes->data, bytes->size);
    if (rc != PDG_OK) {
        return rc;
    }
    bytes->flushed += (uint64_t)bytes->size;
    bytes->size = 0u;
    return PDG_OK;
}

static int bytes_append(PdzPdfBytes *bytes, const void *data, size_t size)
{
    int rc;
    if (!size) {
        return PDG_OK;
    }
    if (!data) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    rc = bytes_reserve(bytes, size);
    if (rc != PDG_OK) {
        return rc;
    }
    memcpy(bytes->data + bytes->size, data, size);
    bytes->size += size;
    if (bytes->flush && bytes->size >= PDZ_PDF_STAGE_BYTES) {
        rc = bytes_flush_all(bytes);
        if (rc != PDG_OK) {
            return rc;
        }
    }
    return PDG_OK;
}

static int bytes_append_text(PdzPdfBytes *bytes, const char *text)
{
    return bytes_append(bytes, text, text ? strlen(text) : 0u);
}

static int bytes_appendf(PdzPdfBytes *bytes, const char *format, ...)
{
    va_list args;
    va_list copy;
    int length;
    char stack[256];
    char *dynamic = NULL;
    int rc;

    if (!bytes || !format) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    va_start(args, format);
    va_copy(copy, args);
    length = vsnprintf(stack, sizeof(stack), format, copy);
    va_end(copy);
    if (length < 0) {
        va_end(args);
        return PDG_ERR_FORMAT;
    }
    if ((size_t)length < sizeof(stack)) {
        rc = bytes_append(bytes, stack, (size_t)length);
        va_end(args);
        return rc;
    }
    dynamic = (char *)malloc((size_t)length + 1u);
    if (!dynamic) {
        va_end(args);
        return PDG_ERR_OUT_OF_MEMORY;
    }
    (void)vsnprintf(dynamic, (size_t)length + 1u, format, args);
    va_end(args);
    rc = bytes_append(bytes, dynamic, (size_t)length);
    free(dynamic);
    return rc;
}

static int pdf_begin_object(PdzPdfBytes *pdf, size_t *offsets,
                            size_t object_count, size_t object_number)
{
    int rc;
    if (!pdf || !offsets || object_number == 0u || object_number > object_count) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    rc = bytes_flush_all(pdf);
    if (rc != PDG_OK) {
        return rc;
    }
    offsets[object_number] = (size_t)bytes_total(pdf);
    return bytes_appendf(pdf, "%zu 0 obj\n", object_number);
}

static int pdf_end_object(PdzPdfBytes *pdf)
{
    return bytes_append_text(pdf, "\nendobj\n");
}

static int image_kind_is_jpeg(int kind)
{
    return kind == QW_PAGE_IMAGE_JPEG;
}

static int image_kind_is_ccitt(int kind)
{
    return kind == QW_PAGE_IMAGE_CCITT;
}

static int image_kind_is_jp2(int kind)
{
    return kind == QW_PAGE_IMAGE_JP2 ||
           kind == QW_PAGE_IMAGE_JP2_ALPHA ||
           kind == QW_PAGE_IMAGE_JP2_ALPHA_PREMULTIPLIED;
}

/* DCTDecode carries the JPEG's own component count, so the PDF colour space
 * must agree with the frame header: SSReader stores most book pages as
 * single-component grey JPEGs, and declaring /DeviceRGB for those makes a
 * conforming reader decode three samples per pixel and lose the page body.
 * The scan also reports whether the stream carries an Adobe APP14 marker,
 * which is the only reliable signal that a CMYK frame needs the inverted
 * /Decode array. */
static const char *jpeg_color_space(const uint8_t *data, size_t size,
                                    int *invert_cmyk)
{
    size_t offset;
    int adobe = 0;
    if (invert_cmyk) {
        *invert_cmyk = 0;
    }
    if (!data || size < 4u || data[0] != 0xffu || data[1] != 0xd8u) {
        return "/DeviceRGB";
    }
    offset = 2u;
    while (offset + 9u < size) {
        unsigned marker;
        unsigned length;
        if (data[offset] != 0xffu) {
            ++offset;
            continue;
        }
        marker = data[offset + 1u];
        if (marker == 0xffu) {
            ++offset;
            continue;
        }
        if (marker == 0xd8u || marker == 0x01u ||
            (marker >= 0xd0u && marker <= 0xd7u)) {
            offset += 2u;
            continue;
        }
        if (marker == 0xd9u || marker == 0xdau) {
            return "/DeviceRGB";
        }
        length = ((unsigned)data[offset + 2u] << 8) | (unsigned)data[offset + 3u];
        if (length < 2u) {
            return "/DeviceRGB";
        }
        if (marker == 0xeeu && length >= 7u && offset + 7u < size &&
            memcmp(data + offset + 4u, "Adobe", 5u) == 0) {
            adobe = 1;
        }
        if (marker >= 0xc0u && marker <= 0xcfu &&
            marker != 0xc4u && marker != 0xc8u && marker != 0xccu) {
            unsigned components = data[offset + 9u];
            if (components == 1u) {
                return "/DeviceGray";
            }
            if (components == 4u) {
                if (invert_cmyk) {
                    *invert_cmyk = adobe;
                }
                return "/DeviceCMYK";
            }
            return "/DeviceRGB";
        }
        offset += 2u + length;
    }
    return "/DeviceRGB";
}

/* The dictionary /Width and /Height are authoritative for a DCTDecode image,
 * so a caller that disagrees with the frame header would get a shifted or
 * truncated page. The frame wins. */
static int jpeg_frame_dimensions(const uint8_t *data, size_t size,
                                 uint32_t *width, uint32_t *height)
{
    size_t offset;
    if (!data || size < 4u || data[0] != 0xffu || data[1] != 0xd8u) {
        return 0;
    }
    offset = 2u;
    while (offset + 9u < size) {
        unsigned marker;
        unsigned length;
        if (data[offset] != 0xffu) {
            ++offset;
            continue;
        }
        marker = data[offset + 1u];
        if (marker == 0xffu) {
            ++offset;
            continue;
        }
        if (marker == 0xd8u || marker == 0x01u ||
            (marker >= 0xd0u && marker <= 0xd7u)) {
            offset += 2u;
            continue;
        }
        if (marker == 0xd9u || marker == 0xdau) {
            return 0;
        }
        length = ((unsigned)data[offset + 2u] << 8) | (unsigned)data[offset + 3u];
        if (length < 2u) {
            return 0;
        }
        if (marker >= 0xc0u && marker <= 0xcfu &&
            marker != 0xc4u && marker != 0xc8u && marker != 0xccu) {
            uint32_t h = ((uint32_t)data[offset + 5u] << 8) | data[offset + 6u];
            uint32_t w = ((uint32_t)data[offset + 7u] << 8) | data[offset + 8u];
            if (!w || !h) {
                return 0;
            }
            *width = w;
            *height = h;
            return 1;
        }
        offset += 2u + length;
    }
    return 0;
}

static int image_dimensions(const PdzPdfImage *image, double *width, double *height)
{
    uint32_t frame_width;
    uint32_t frame_height;
    if (!image || !width || !height || !image->data || !image->size ||
        !image->width || !image->height) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    if (image_kind_is_jpeg(image->kind) &&
        jpeg_frame_dimensions(image->data, image->size, &frame_width, &frame_height)) {
        *width = (double)frame_width;
        *height = (double)frame_height;
        return PDG_OK;
    }
    *width = (double)image->width;
    *height = (double)image->height;
    return PDG_OK;
}

static int append_raw_image_data(PdzPdfBytes *pdf, const PdzPdfImage *image,
                                 size_t expected_row_bytes)
{
    size_t expected;
    if (!image || image->kind == QW_PAGE_IMAGE_JPEG ||
        image->kind == QW_PAGE_IMAGE_CCITT || image_kind_is_jp2(image->kind)) {
        return bytes_append(pdf, image ? image->data : NULL, image ? image->size : 0u);
    }
    if (image->height > SIZE_MAX / expected_row_bytes) {
        return PDG_ERR_RESOURCE_LIMIT;
    }
    expected = (size_t)image->height * expected_row_bytes;
    if (image->size < expected) {
        return PDG_ERR_FORMAT;
    }
    return bytes_append(pdf, image->data, expected);
}

static int pdf_write_image(PdzPdfBytes *pdf, size_t *offsets, size_t object_count,
                           size_t object_number, const PdzPdfImage *image)
{
    const char *color_space;
    size_t row_bytes;
    size_t data_size;
    int bits;
    int rc;

    if (!pdf || !offsets || !image || !image->data || !image->size ||
        !image->width || !image->height) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    rc = pdf_begin_object(pdf, offsets, object_count, object_number);
    if (rc != PDG_OK) {
        return rc;
    }
    if (image_kind_is_jpeg(image->kind)) {
        int invert_cmyk = 0;
        uint32_t frame_width = image->width;
        uint32_t frame_height = image->height;
        color_space = jpeg_color_space(image->data, image->size, &invert_cmyk);
        bits = 8;
        data_size = image->size;
        (void)jpeg_frame_dimensions(image->data, image->size, &frame_width, &frame_height);
        if (strcmp(color_space, "/DeviceCMYK") == 0 && invert_cmyk) {
            rc = bytes_appendf(pdf,
                               "<< /Type /XObject /Subtype /Image /Width %u /Height %u "
                               "/ColorSpace %s /BitsPerComponent %d /Filter /DCTDecode "
                               "/Decode [1 0 1 0 1 0 1 0] /Length %zu >>\nstream\n",
                               frame_width, frame_height, color_space, bits, data_size);
        } else {
            rc = bytes_appendf(pdf,
                               "<< /Type /XObject /Subtype /Image /Width %u /Height %u "
                               "/ColorSpace %s /BitsPerComponent %d /Filter /DCTDecode "
                               "/Length %zu >>\nstream\n",
                               frame_width, frame_height, color_space, bits, data_size);
        }
        if (rc == PDG_OK) {
            rc = bytes_append(pdf, image->data, image->size);
        }
    } else if (image_kind_is_ccitt(image->kind)) {
        bits = 1;
        data_size = image->size;
        /* Callers hand over ITU-T T.6 streams whose black runs are 1 bits (the
         * pdg_render_to_ccitt output and every TIFF/libtiff encoder). PDF's
         * default is the opposite polarity, so BlackIs1 must be explicit or
         * every page renders as a negative. */
        rc = bytes_appendf(pdf,
                           "<< /Type /XObject /Subtype /Image /Width %u /Height %u "
                           "/ColorSpace /DeviceGray /BitsPerComponent 1 "
                           "/Filter /CCITTFaxDecode /DecodeParms << /K -1 /Columns %u "
                           "/Rows %u /BlackIs1 true >> /Length %zu >>\nstream\n",
                           image->width, image->height, image->width, image->height,
                           data_size);
        if (rc == PDG_OK) {
            rc = bytes_append(pdf, image->data, image->size);
        }
    } else if (image_kind_is_jp2(image->kind)) {
        data_size = image->size;
        rc = bytes_appendf(pdf,
                           "<< /Type /XObject /Subtype /Image /Width %u /Height %u "
                           "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /JPXDecode "
                           "/Length %zu >>\nstream\n",
                           image->width, image->height, data_size);
        if (rc == PDG_OK) {
            rc = bytes_append(pdf, image->data, image->size);
        }
    } else {
        if (image->kind == QW_PAGE_IMAGE_GRAY8) {
            row_bytes = image->width;
            color_space = "/DeviceGray";
        } else if (image->kind == QW_PAGE_IMAGE_RGB24) {
            if (image->width > SIZE_MAX / 3u) {
                return PDG_ERR_RESOURCE_LIMIT;
            }
            row_bytes = (size_t)image->width * 3u;
            color_space = "/DeviceRGB";
        } else {
            return PDG_ERR_UNSUPPORTED;
        }
        if (image->height > SIZE_MAX / row_bytes) {
            return PDG_ERR_RESOURCE_LIMIT;
        }
        data_size = (size_t)image->height * row_bytes;
        rc = bytes_appendf(pdf,
                           "<< /Type /XObject /Subtype /Image /Width %u /Height %u "
                           "/ColorSpace %s /BitsPerComponent 8 /Length %zu >>\nstream\n",
                           image->width, image->height, color_space, data_size);
        if (rc == PDG_OK) {
            rc = append_raw_image_data(pdf, image, row_bytes);
        }
    }
    if (rc == PDG_OK) {
        rc = bytes_append_text(pdf, "\nendstream\n");
    }
    if (rc == PDG_OK) {
        rc = pdf_end_object(pdf);
    }
    return rc;
}

static int pdf_page_geometry(const PdzPdfImage *image, int page_mode,
                             double *page_width, double *page_height,
                             double *draw_x, double *draw_y,
                             double *draw_width, double *draw_height)
{
    double image_width;
    double image_height;
    if (image_dimensions(image, &image_width, &image_height) != PDG_OK) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    if (page_mode == PDZ_PDF_PAGE_IMAGE) {
        *page_width = image_width * 72.0 / PDZ_PDF_DPI;
        *page_height = image_height * 72.0 / PDZ_PDF_DPI;
        *draw_width = *page_width;
        *draw_height = *page_height;
        *draw_x = 0.0;
        *draw_y = 0.0;
        return PDG_OK;
    }
    /* A4 portrait: keep the aspect ratio, fit by the tighter axis and centre.
     * The image's physical size is its pixel size at PDZ_PDF_DPI; the content
     * stream must use the same units as the page box, otherwise the image
     * overflows by DPI/72. */
    {
        double image_pt_width = image_width * 72.0 / PDZ_PDF_DPI;
        double image_pt_height = image_height * 72.0 / PDZ_PDF_DPI;
        double page_w = 595.276;
        double page_h = 841.89;
        double scale_w = page_w / image_pt_width;
        double scale_h = page_h / image_pt_height;
        double scale = scale_w < scale_h ? scale_w : scale_h;
        *page_width = page_w;
        *page_height = page_h;
        *draw_width = image_pt_width * scale;
        *draw_height = image_pt_height * scale;
        *draw_x = (page_w - *draw_width) / 2.0;
        *draw_y = (page_h - *draw_height) / 2.0;
    }
    return PDG_OK;
}

static int pdf_write_header(PdzPdfBytes *pdf, size_t *offsets, size_t object_count)
{
    int rc = bytes_append_text(pdf, "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n");
    if (rc == PDG_OK) {
        rc = pdf_begin_object(pdf, offsets, object_count, 1u);
    }
    if (rc == PDG_OK) {
        rc = bytes_append_text(pdf, "<< /Type /Catalog /Pages 2 0 R >>");
    }
    if (rc == PDG_OK) {
        rc = pdf_end_object(pdf);
    }
    return rc;
}

static int pdf_write_page(PdzPdfBytes *pdf, size_t *offsets, size_t object_count,
                          size_t index, const PdzPdfImage *image, int page_mode)
{
    double page_width;
    double page_height;
    double draw_width;
    double draw_height;
    double draw_x;
    double draw_y;
    size_t page_id = 3u + index * 3u;
    size_t content_id = page_id + 1u;
    size_t image_id = page_id + 2u;
    char content[256];
    int content_length;
    int rc;

    rc = pdf_page_geometry(image, page_mode, &page_width, &page_height,
                           &draw_x, &draw_y, &draw_width, &draw_height);
    if (rc != PDG_OK) {
        return rc;
    }
    rc = pdf_begin_object(pdf, offsets, object_count, page_id);
    if (rc == PDG_OK) {
        rc = bytes_appendf(pdf,
                           "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.3f %.3f] "
                           "/Resources << /XObject << /Im0 %zu 0 R >> >> "
                           "/Contents %zu 0 R >>",
                           page_width, page_height, image_id, content_id);
    }
    if (rc == PDG_OK) {
        rc = pdf_end_object(pdf);
    }
    if (rc != PDG_OK) {
        return rc;
    }
    content_length = snprintf(content, sizeof(content),
                              "q %.3f 0 0 %.3f %.3f %.3f cm /Im0 Do Q\n",
                              draw_width, draw_height, draw_x, draw_y);
    if (content_length < 0 || (size_t)content_length >= sizeof(content)) {
        return PDG_ERR_FORMAT;
    }
    rc = pdf_begin_object(pdf, offsets, object_count, content_id);
    if (rc == PDG_OK) {
        rc = bytes_appendf(pdf, "<< /Length %d >>\nstream\n", content_length);
    }
    if (rc == PDG_OK) {
        rc = bytes_append(pdf, content, (size_t)content_length);
    }
    if (rc == PDG_OK) {
        rc = bytes_append_text(pdf, "endstream");
    }
    if (rc == PDG_OK) {
        rc = pdf_end_object(pdf);
    }
    if (rc == PDG_OK) {
        rc = pdf_write_image(pdf, offsets, object_count, image_id, image);
    }
    return rc;
}

static int pdf_write_pages_object(PdzPdfBytes *pdf, size_t *offsets,
                                  size_t object_count, size_t page_count)
{
    size_t index;
    int rc = pdf_begin_object(pdf, offsets, object_count, 2u);
    if (rc == PDG_OK) {
        rc = bytes_appendf(pdf, "<< /Type /Pages /Count %zu /Kids [", page_count);
    }
    for (index = 0u; rc == PDG_OK && index < page_count; ++index) {
        rc = bytes_appendf(pdf, "%zu 0 R ", 3u + index * 3u);
    }
    if (rc == PDG_OK) {
        rc = bytes_append_text(pdf, "] >>");
    }
    if (rc == PDG_OK) {
        rc = pdf_end_object(pdf);
    }
    return rc;
}

static int pdf_write_trailer(PdzPdfBytes *pdf, size_t *offsets, size_t object_count)
{
    size_t index;
    uint64_t xref;
    int rc = bytes_flush_all(pdf);
    if (rc != PDG_OK) {
        return rc;
    }
    xref = bytes_total(pdf);
    rc = bytes_appendf(pdf, "xref\n0 %zu\n0000000000 65535 f \n", object_count + 1u);
    for (index = 1u; rc == PDG_OK && index <= object_count; ++index) {
        rc = bytes_appendf(pdf, "%010zu 00000 n \n", offsets[index]);
    }
    if (rc == PDG_OK) {
        rc = bytes_appendf(pdf,
                           "trailer\n<< /Size %zu /Root 1 0 R >>\nstartxref\n%llu\n%%EOF\n",
                           object_count + 1u, (unsigned long long)xref);
    }
    return rc;
}

static int pdf_write_images(const PdzPdfImage *images, size_t count, int page_mode,
                            uint8_t **out_pdf, size_t *out_size, size_t *out_pages)
{
    PdzPdfBytes pdf = {0};
    size_t object_count;
    size_t *offsets = NULL;
    size_t index;
    int rc;

    if (!out_pdf || !out_size || (count && !images)) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    *out_pdf = NULL;
    *out_size = 0u;
    if (out_pages) {
        *out_pages = 0u;
    }
    if (!count) {
        return PDG_ERR_EMPTY;
    }
    if (count > (SIZE_MAX - 2u) / 3u) {
        return PDG_ERR_RESOURCE_LIMIT;
    }
    object_count = 2u + count * 3u;
    offsets = (size_t *)calloc(object_count + 1u, sizeof(*offsets));
    if (!offsets) {
        return PDG_ERR_OUT_OF_MEMORY;
    }
    rc = pdf_write_header(&pdf, offsets, object_count);
    for (index = 0u; rc == PDG_OK && index < count; ++index) {
        rc = pdf_write_page(&pdf, offsets, object_count, index, &images[index], page_mode);
    }
    if (rc == PDG_OK) {
        rc = pdf_write_pages_object(&pdf, offsets, object_count, count);
    }
    if (rc == PDG_OK) {
        rc = pdf_write_trailer(&pdf, offsets, object_count);
    }
    if (rc == PDG_OK) {
        rc = bytes_flush_all(&pdf);
    }
    if (rc == PDG_OK) {
        *out_pdf = pdf.data;
        *out_size = pdf.size;
        if (out_pages) {
            *out_pages = count;
        }
        pdf.data = NULL;
    }
    free(pdf.data);
    free(offsets);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Streaming writer                                                    */
/* ------------------------------------------------------------------ */

struct PdzPdfWriter {
    PdzPdfBytes pdf;
    size_t *offsets;
    size_t offset_capacity;   /* allocated entries; object numbers are 1-based */
    size_t object_count;      /* 2 + 3 * pages */
    size_t page_count;
    int page_mode;
    int failed;
};

static int pdf_writer_reserve(PdzPdfWriter *writer, size_t object_count)
{
    size_t capacity = object_count + 1u;
    size_t *grown;
    if (capacity <= writer->offset_capacity) {
        writer->object_count = object_count;
        return PDG_OK;
    }
    if (capacity > SIZE_MAX / sizeof(*grown)) {
        return PDG_ERR_RESOURCE_LIMIT;
    }
    grown = (size_t *)realloc(writer->offsets, capacity * sizeof(*grown));
    if (!grown) {
        return PDG_ERR_OUT_OF_MEMORY;
    }
    memset(grown + writer->offset_capacity, 0,
           (capacity - writer->offset_capacity) * sizeof(*grown));
    writer->offsets = grown;
    writer->offset_capacity = capacity;
    writer->object_count = object_count;
    return PDG_OK;
}

PDG_API int pdz_pdf_writer_create(int page_mode, PdzPdfWriteFn write_fn,
                                  void *context, PdzPdfWriter **out_writer)
{
    PdzPdfWriter *writer;
    int rc;

    if (!out_writer || !write_fn) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    *out_writer = NULL;
    if (page_mode != PDZ_PDF_PAGE_A4 && page_mode != PDZ_PDF_PAGE_IMAGE) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    writer = (PdzPdfWriter *)calloc(1u, sizeof(*writer));
    if (!writer) {
        return PDG_ERR_OUT_OF_MEMORY;
    }
    writer->page_mode = page_mode;
    writer->pdf.flush = write_fn;
    writer->pdf.flush_context = context;
    rc = pdf_writer_reserve(writer, 2u);
    if (rc == PDG_OK) {
        rc = pdf_write_header(&writer->pdf, writer->offsets, writer->object_count);
    }
    if (rc != PDG_OK) {
        pdz_pdf_writer_destroy(writer);
        return rc;
    }
    *out_writer = writer;
    return PDG_OK;
}

PDG_API int pdz_pdf_writer_add_page(PdzPdfWriter *writer, const PdzPdfImage *image)
{
    size_t object_count;
    int rc;

    if (!writer || !image) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    if (writer->failed) {
        return PDG_ERR_FORMAT;
    }
    if (writer->page_count >= (SIZE_MAX - 2u) / 3u) {
        return PDG_ERR_RESOURCE_LIMIT;
    }
    object_count = 2u + (writer->page_count + 1u) * 3u;
    rc = pdf_writer_reserve(writer, object_count);
    if (rc == PDG_OK) {
        rc = pdf_write_page(&writer->pdf, writer->offsets, writer->object_count,
                            writer->page_count, image, writer->page_mode);
    }
    if (rc != PDG_OK) {
        writer->failed = 1;
        return rc;
    }
    writer->page_count += 1u;
    return PDG_OK;
}

PDG_API int pdz_pdf_writer_finish(PdzPdfWriter *writer, size_t *out_pages)
{
    int rc;

    if (out_pages) {
        *out_pages = 0u;
    }
    if (!writer) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    if (writer->failed) {
        return PDG_ERR_FORMAT;
    }
    if (!writer->page_count) {
        return PDG_ERR_EMPTY;
    }
    rc = pdf_write_pages_object(&writer->pdf, writer->offsets,
                                writer->object_count, writer->page_count);
    if (rc == PDG_OK) {
        rc = pdf_write_trailer(&writer->pdf, writer->offsets, writer->object_count);
    }
    if (rc == PDG_OK) {
        rc = bytes_flush_all(&writer->pdf);
    }
    if (rc != PDG_OK) {
        writer->failed = 1;
        return rc;
    }
    if (out_pages) {
        *out_pages = writer->page_count;
    }
    return PDG_OK;
}

PDG_API void pdz_pdf_writer_destroy(PdzPdfWriter *writer)
{
    if (!writer) {
        return;
    }
    free(writer->pdf.data);
    free(writer->offsets);
    free(writer);
}

static int name_is_digits(const char *name, size_t length)
{
    size_t index;
    if (!name || length == 0u) {
        return 0;
    }
    for (index = 0u; index < length; ++index) {
        if (name[index] < '0' || name[index] > '9') {
            return 0;
        }
    }
    return 1;
}

static int member_is_body(const PdzMember *member)
{
    size_t length;
    if (!member) {
        return 0;
    }
    length = strnlen(member->name, PDZ_NAME_SIZE);
    return length == 6u && name_is_digits(member->name, length);
}

PDG_API int pdz_pdfpdg_is_member(const void *member, size_t size)
{
    PdgHeader header;
    int rc;

    if (!member) return 0;
    rc = pdg_parse_header((const uint8_t *)member, size, &header);
    return rc == PDG_OK && header.type == 0x1au &&
           header.data_offset == PDG_HEADER_SIZE && header.data_size != 0u;
}

PDG_API int pdz_pdfpdg_decode_member(const void *member, size_t size,
                                     PdzBuffer *out)
{
    PdgBuffer decoded;
    uint8_t *pdf = NULL;
    size_t pdf_size = 0u;
    int rc;

    if (!member || !out || !size) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    if (!pdz_pdfpdg_is_member(member, size)) {
        return PDG_ERR_FORMAT;
    }
    memset(&decoded, 0, sizeof(decoded));
    rc = pdg_decrypt_memory(member, size, &decoded);
    if (rc != PDG_OK) {
        pdg_buffer_free(&decoded);
        return rc;
    }
    if (decoded.header.type != 0x1au || !decoded.data || decoded.size == 0u) {
        pdg_buffer_free(&decoded);
        return PDG_ERR_FORMAT;
    }
    rc = pdz_inflate_zlib(decoded.data, decoded.size, &pdf, &pdf_size);
    pdg_buffer_free(&decoded);
    if (rc != PDG_OK) {
        free(pdf);
        return PDG_ERR_FORMAT;
    }
    if (pdf_size < 4u || memcmp(pdf, "%PDF", 4u) != 0) {
        free(pdf);
        return PDG_ERR_FORMAT;
    }
    out->data = pdf;
    out->size = pdf_size;
    return PDG_OK;
}

static int member_is_attachment(const PdzMember *member)
{
    size_t length;
    if (!member) {
        return 0;
    }
    length = strnlen(member->name, PDZ_NAME_SIZE);
    return length >= 4u && strncmp(member->name, "att", 3u) == 0 &&
           name_is_digits(member->name + 3u, length - 3u);
}

PDG_API int pdz_pdf_member_selected(const PdzMember *member, int rule)
{
    if (!member) {
        return 0;
    }
    switch (rule) {
    case PDZ_PDF_PAGES_BODY:
        return member_is_body(member);
    case PDZ_PDF_PAGES_BODY_ATT:
        return member_is_body(member) || member_is_attachment(member);
    case PDZ_PDF_PAGES_AUTO:
    case PDZ_PDF_PAGES_ALL:
    default:
        /* AUTO is only a request; pdz_pdf_resolve_rule() turns it into a
         * concrete rule. Treating it as ALL keeps a direct call predictable. */
        return 1;
    }
}

PDG_API int pdz_pdf_from_images(const PdzPdfImage *images, size_t count,
                                int page_mode, uint8_t **out_pdf,
                                size_t *out_size, size_t *out_pages)
{
    if (page_mode != PDZ_PDF_PAGE_A4 && page_mode != PDZ_PDF_PAGE_IMAGE) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    return pdf_write_images(images, count, page_mode, out_pdf, out_size, out_pages);
}

PDG_API void pdz_pdf_free(void *pdf)
{
    free(pdf);
}

PDG_API uint32_t pdz_pdf_meta_pages(const void *xml, size_t size)
{
    const char *bytes = (const char *)xml;
    const char *tag;
    const char *cursor;
    uint64_t value = 0u;
    if (!bytes || !size) {
        return 0u;
    }
    tag = (const char *)memchr(bytes, '<', size);
    while (tag) {
        if ((size_t)(tag - bytes) + 7u <= size &&
            strncmp(tag, "<pages>", 7u) == 0) {
            cursor = tag + 7;
            while ((size_t)(cursor - bytes) < size && *cursor >= '0' && *cursor <= '9') {
                value = value * 10u + (uint64_t)(*cursor - '0');
                if (value > UINT32_MAX) {
                    return 0u;
                }
                ++cursor;
            }
            return (uint32_t)value;
        }
        {
            size_t offset = (size_t)(tag - bytes) + 1u;
            if (offset >= size) {
                break;
            }
            tag = (const char *)memchr(bytes + offset, '<', size - offset);
        }
    }
    return 0u;
}

PDG_API size_t pdz_pdf_rule_count(const PdzMember *members, size_t count, int rule)
{
    size_t index;
    size_t selected = 0u;
    if (rule < PDZ_PDF_PAGES_AUTO || rule > PDZ_PDF_PAGES_ALL) {
        return 0u;
    }
    for (index = 0u; index < count; ++index) {
        if (pdz_pdf_member_selected(&members[index], rule)) {
            ++selected;
        }
    }
    return selected;
}

/* Declared page count from a container's meta.xml member, or 0 when absent. */
static uint32_t pdz_pdf_declared_pages(const void *data, size_t size,
                                       const PdzMember *members, size_t count,
                                       const char *key_string)
{
    size_t index;
    if (!data || !members) {
        return 0u;
    }
    for (index = 0u; index < count; ++index) {
        PdzBuffer payload;
        uint32_t pages;
        if (strcmp(members[index].name, "meta.xml") != 0) {
            continue;
        }
        memset(&payload, 0, sizeof(payload));
        if (pdz_member_payload(data, size, &members[index], key_string,
                               &payload) != PDG_OK) {
            return 0u;
        }
        pages = pdz_pdf_meta_pages(payload.data, payload.size);
        pdz_buffer_free(&payload);
        return pages;
    }
    return 0u;
}

PDG_API int pdz_pdf_resolve_rule(const void *data, size_t size,
                                 const PdzMember *members, size_t count,
                                 const char *key_string, int requested_rule)
{
    static const int preference[] = {
        PDZ_PDF_PAGES_BODY_ATT,
        PDZ_PDF_PAGES_BODY,
        PDZ_PDF_PAGES_ALL,
    };
    uint32_t declared;
    size_t index;

    if (requested_rule != PDZ_PDF_PAGES_AUTO) {
        if (requested_rule < PDZ_PDF_PAGES_AUTO ||
            requested_rule > PDZ_PDF_PAGES_ALL) {
            return PDG_ERR_INVALID_ARGUMENT;
        }
        return requested_rule;
    }
    declared = pdz_pdf_declared_pages(data, size, members, count, key_string);
    if (declared > 0u) {
        for (index = 0u; index < sizeof(preference) / sizeof(preference[0]);
             ++index) {
            if (pdz_pdf_rule_count(members, count, preference[index]) ==
                (size_t)declared) {
                return preference[index];
            }
        }
    }
    return PDZ_PDF_PAGES_ALL;
}

PDG_API const char *pdz_pdf_page_rule_name(int rule)
{
    switch (rule) {
    case PDZ_PDF_PAGES_AUTO: return "auto";
    case PDZ_PDF_PAGES_BODY: return "body";
    case PDZ_PDF_PAGES_BODY_ATT: return "body+attachments";
    case PDZ_PDF_PAGES_ALL: return "all";
    default: return "unknown";
    }
}

PDG_API const char *pdz_pdf_route_name(int route)
{
    switch (route) {
    case PDZ_PDF_ROUTE_NONE: return "none";
    case PDZ_PDF_ROUTE_MEMBERS: return "native-members";
    case PDZ_PDF_ROUTE_IMAGES: return "images";
    default: return "unknown";
    }
}

PDG_API int pdz_pdf_merge(const uint8_t *const *pdfs, const size_t *sizes,
                          size_t count, uint8_t **out_pdf, size_t *out_size,
                          size_t *out_pages)
{
    (void)pdfs;
    (void)sizes;
    (void)count;
    if (!out_pdf || !out_size) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    *out_pdf = NULL;
    *out_size = 0u;
    if (out_pages) {
        *out_pages = 0u;
    }
    return PDG_ERR_UNSUPPORTED;
}

PDG_API int pdz_pdf_from_pdg_files(const uint8_t *const *blobs,
                                   const size_t *sizes, size_t count,
                                   int page_mode, uint8_t **out_pdf,
                                   size_t *out_size, size_t *out_pages,
                                   size_t *out_skipped)
{
    PdzPdfImage *images = NULL;
    PdgImage *rendered = NULL;
    size_t image_count = 0u;
    size_t index;
    int rc = PDG_OK;

    if (!blobs || !sizes || !out_pdf || !out_size ||
        (page_mode != PDZ_PDF_PAGE_A4 && page_mode != PDZ_PDF_PAGE_IMAGE)) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    *out_pdf = NULL;
    *out_size = 0u;
    if (out_pages) *out_pages = 0u;
    if (out_skipped) *out_skipped = 0u;
    if (count) {
        images = (PdzPdfImage *)calloc(count, sizeof(*images));
        rendered = (PdgImage *)calloc(count, sizeof(*rendered));
        if (!images || !rendered) {
            free(images);
            free(rendered);
            return PDG_ERR_OUT_OF_MEMORY;
        }
    }
    for (index = 0u; index < count; ++index) {
        rc = pdg_render_memory(blobs[index], sizes[index], &rendered[image_count]);
        if (rc != PDG_OK) {
            if (out_skipped) ++*out_skipped;
            continue;
        }
        images[image_count].data = rendered[image_count].pixels;
        images[image_count].size = rendered[image_count].size;
        images[image_count].kind = rendered[image_count].format == PDG_PIXEL_GRAY8
            ? QW_PAGE_IMAGE_GRAY8 : QW_PAGE_IMAGE_RGB24;
        images[image_count].width = rendered[image_count].width;
        images[image_count].height = rendered[image_count].height;
        ++image_count;
    }
    rc = pdz_pdf_from_images(images, image_count, page_mode,
                             out_pdf, out_size, out_pages);
    for (index = 0u; index < image_count; ++index) {
        pdg_image_free(&rendered[index]);
    }
    free(images);
    free(rendered);
    return rc;
}

PDG_API int pdz_pdf_from_pdz(const void *data, size_t size,
                             const char *key_string, int page_rule,
                             int page_mode, uint8_t **out_pdf, size_t *out_size,
                             PdzPdfReport *report)
{
    PdzMember *members = NULL;
    PdzPdfImage *images = NULL;
    PdgImage *rendered = NULL;
    size_t member_count = 0u;
    size_t image_count = 0u;
    size_t index;
    int rc;

    if (!data || !size || !out_pdf || !out_size ||
        (page_mode != PDZ_PDF_PAGE_A4 && page_mode != PDZ_PDF_PAGE_IMAGE)) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    *out_pdf = NULL;
    *out_size = 0u;
    if (report) {
        memset(report, 0, sizeof(*report));
        report->route = PDZ_PDF_ROUTE_IMAGES;
        report->page_rule = page_rule;
        report->members_total = 0u;
    }
    rc = pdz_members(data, size, NULL, 0u, &member_count);
    if (rc != PDG_OK || !member_count) {
        return rc == PDG_OK ? PDG_ERR_EMPTY : rc;
    }
    members = (PdzMember *)calloc(member_count, sizeof(*members));
    images = (PdzPdfImage *)calloc(member_count, sizeof(*images));
    rendered = (PdgImage *)calloc(member_count, sizeof(*rendered));
    if (!members || !images || !rendered) {
        free(members);
        free(images);
        free(rendered);
        return PDG_ERR_OUT_OF_MEMORY;
    }
    rc = pdz_members(data, size, members, member_count, &member_count);
    if (rc != PDG_OK) {
        goto cleanup;
    }
    rc = pdz_pdf_resolve_rule(data, size, members, member_count, key_string,
                              page_rule);
    if (rc < 0) {
        goto cleanup;
    }
    page_rule = rc;
    if (report) {
        size_t rule_index;
        report->members_total = member_count;
        report->page_rule = page_rule;
        report->declared_pages = pdz_pdf_declared_pages(data, size, members,
                                                        member_count,
                                                        key_string);
        for (rule_index = 0u;
             rule_index < sizeof(report->rule_counts) /
                              sizeof(report->rule_counts[0]);
             ++rule_index) {
            report->rule_counts[rule_index] =
                pdz_pdf_rule_count(members, member_count, (int)rule_index);
        }
    }
    for (index = 0u; index < member_count; ++index) {
        PdzBuffer payload = {0};
        if (!pdz_pdf_member_selected(&members[index], page_rule)) {
            continue;
        }
        rc = pdz_member_payload(data, size, &members[index], key_string, &payload);
        if (rc != PDG_OK) {
            if (report) ++report->skipped;
            continue;
        }
        if (pdz_pdfpdg_is_member(payload.data, payload.size)) {
            size_t pdf_members = 1u;
            size_t other;
            pdz_buffer_free(&payload);
            /* Count the remaining native PDF members so the report tells a
             * caller how many pages the composition-layer merger would add. */
            for (other = index + 1u; other < member_count; ++other) {
                PdzBuffer extra = {0};
                if (!pdz_pdf_member_selected(&members[other], page_rule)) {
                    continue;
                }
                if (pdz_member_payload(data, size, &members[other], key_string,
                                       &extra) != PDG_OK) {
                    continue;
                }
                if (pdz_pdfpdg_is_member(extra.data, extra.size)) {
                    ++pdf_members;
                }
                pdz_buffer_free(&extra);
            }
            rc = PDG_ERR_UNSUPPORTED;
            if (report) {
                report->route = PDZ_PDF_ROUTE_MEMBERS;
                report->pdf_members = pdf_members;
                snprintf(report->note, sizeof(report->note),
                         "%zu native PDF member(s) need the composition-layer "
                         "PDF merger",
                         pdf_members);
            }
            goto cleanup;
        }
        rc = pdg_render_memory(payload.data, payload.size, &rendered[image_count]);
        pdz_buffer_free(&payload);
        if (rc != PDG_OK) {
            if (report) ++report->skipped;
            continue;
        }
        images[image_count].data = rendered[image_count].pixels;
        images[image_count].size = rendered[image_count].size;
        images[image_count].kind = rendered[image_count].format == PDG_PIXEL_GRAY8
            ? QW_PAGE_IMAGE_GRAY8 : QW_PAGE_IMAGE_RGB24;
        images[image_count].width = rendered[image_count].width;
        images[image_count].height = rendered[image_count].height;
        ++image_count;
    }
    rc = pdz_pdf_from_images(images, image_count, page_mode,
                             out_pdf, out_size,
                             report ? &report->pages_written : NULL);
    if (report) {
        report->members_used = image_count;
        report->images_raw = image_count;
    }

cleanup:
    for (index = 0u; index < image_count; ++index) {
        pdg_image_free(&rendered[index]);
    }
    free(members);
    free(images);
    free(rendered);
    return rc;
}
