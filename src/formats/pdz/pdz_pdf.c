#include "pdz_pdf.h"
#include "pdg.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct PdzPdfBytes {
    uint8_t *data;
    size_t size;
    size_t capacity;
} PdzPdfBytes;

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
    if (!pdf || !offsets || object_number == 0u || object_number > object_count) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    offsets[object_number] = pdf->size;
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

static int image_dimensions(const PdzPdfImage *image, double *width, double *height)
{
    if (!image || !width || !height || !image->data || !image->size ||
        !image->width || !image->height) {
        return PDG_ERR_INVALID_ARGUMENT;
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
        color_space = "/DeviceRGB";
        bits = 8;
        data_size = image->size;
        rc = bytes_appendf(pdf,
                           "<< /Type /XObject /Subtype /Image /Width %u /Height %u "
                           "/ColorSpace %s /BitsPerComponent %d /Filter /DCTDecode "
                           "/Length %zu >>\nstream\n",
                           image->width, image->height, color_space, bits, data_size);
        if (rc == PDG_OK) {
            rc = bytes_append(pdf, image->data, image->size);
        }
    } else if (image_kind_is_ccitt(image->kind)) {
        bits = 1;
        data_size = image->size;
        rc = bytes_appendf(pdf,
                           "<< /Type /XObject /Subtype /Image /Width %u /Height %u "
                           "/ColorSpace /DeviceGray /BitsPerComponent 1 "
                           "/Filter /CCITTFaxDecode /DecodeParms << /K -1 /Columns %u "
                           "/Rows %u >> /Length %zu >>\nstream\n",
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

static int pdf_write_images(const PdzPdfImage *images, size_t count, int page_mode,
                            uint8_t **out_pdf, size_t *out_size, size_t *out_pages)
{
    PdzPdfBytes pdf = {0};
    size_t object_count;
    size_t *offsets = NULL;
    size_t *page_ids = NULL;
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
    page_ids = (size_t *)calloc(count, sizeof(*page_ids));
    if (!offsets || !page_ids) {
        free(offsets);
        free(page_ids);
        return PDG_ERR_OUT_OF_MEMORY;
    }
    rc = bytes_append_text(&pdf, "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n");
    if (rc == PDG_OK) {
        rc = pdf_begin_object(&pdf, offsets, object_count, 1u);
    }
    if (rc == PDG_OK) {
        rc = bytes_append_text(&pdf, "<< /Type /Catalog /Pages 2 0 R >>");
    }
    if (rc == PDG_OK) {
        rc = pdf_end_object(&pdf);
    }
    if (rc == PDG_OK) {
        rc = pdf_begin_object(&pdf, offsets, object_count, 2u);
    }
    if (rc == PDG_OK) {
        rc = bytes_appendf(&pdf, "<< /Type /Pages /Count %zu /Kids [", count);
    }
    for (index = 0u; rc == PDG_OK && index < count; ++index) {
        page_ids[index] = 3u + index * 3u;
        rc = bytes_appendf(&pdf, "%zu 0 R ", page_ids[index]);
    }
    if (rc == PDG_OK) {
        rc = bytes_append_text(&pdf, "] >>");
    }
    if (rc == PDG_OK) {
        rc = pdf_end_object(&pdf);
    }
    for (index = 0u; rc == PDG_OK && index < count; ++index) {
        const PdzPdfImage *image = &images[index];
        double image_width;
        double image_height;
        double page_width;
        double page_height;
        size_t page_id = page_ids[index];
        size_t content_id = page_id + 1u;
        size_t image_id = page_id + 2u;
        if (image_dimensions(image, &image_width, &image_height) != PDG_OK) {
            rc = PDG_ERR_INVALID_ARGUMENT;
            break;
        }
        if (page_mode == PDZ_PDF_PAGE_IMAGE) {
            page_width = image_width * 72.0 / PDZ_PDF_DPI;
            page_height = image_height * 72.0 / PDZ_PDF_DPI;
        } else {
            page_width = 595.276;
            page_height = 841.89;
        }
        rc = pdf_begin_object(&pdf, offsets, object_count, page_id);
        if (rc == PDG_OK) {
            rc = bytes_appendf(&pdf,
                               "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.3f %.3f] "
                               "/Resources << /XObject << /Im0 %zu 0 R >> >> "
                               "/Contents %zu 0 R >>",
                               page_width, page_height, image_id, content_id);
        }
        if (rc == PDG_OK) {
            rc = pdf_end_object(&pdf);
        }
        if (rc == PDG_OK) {
            char content[256];
            int content_length = snprintf(content, sizeof(content),
                                          "q %.3f 0 0 %.3f 0 0 cm /Im0 Do Q\n",
                                          image_width, image_height);
            if (content_length < 0 || (size_t)content_length >= sizeof(content)) {
                rc = PDG_ERR_FORMAT;
            } else {
                rc = pdf_begin_object(&pdf, offsets, object_count, content_id);
                if (rc == PDG_OK) {
                    rc = bytes_appendf(&pdf, "<< /Length %d >>\nstream\n", content_length);
                }
                if (rc == PDG_OK) {
                    rc = bytes_append(&pdf, content, (size_t)content_length);
                }
                if (rc == PDG_OK) {
                    rc = bytes_append_text(&pdf, "endstream");
                }
                if (rc == PDG_OK) {
                    rc = pdf_end_object(&pdf);
                }
            }
        }
        if (rc == PDG_OK) {
            rc = pdf_write_image(&pdf, offsets, object_count, image_id, image);
        }
    }
    if (rc == PDG_OK) {
        size_t xref = pdf.size;
        rc = bytes_appendf(&pdf, "xref\n0 %zu\n0000000000 65535 f \n", object_count + 1u);
        for (index = 1u; rc == PDG_OK && index <= object_count; ++index) {
            rc = bytes_appendf(&pdf, "%010zu 00000 n \n", offsets[index]);
        }
        if (rc == PDG_OK) {
            rc = bytes_appendf(&pdf,
                               "trailer\n<< /Size %zu /Root 1 0 R >>\nstartxref\n%zu\n%%EOF\n",
                               object_count + 1u, xref);
        }
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
    free(page_ids);
    return rc;
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
        if (rule == PDZ_PDF_PAGES_ALL ||
            (rule == PDZ_PDF_PAGES_BODY && member_is_body(&members[index])) ||
            (rule == PDZ_PDF_PAGES_BODY_ATT &&
             (member_is_body(&members[index]) || member_is_attachment(&members[index])))) {
            ++selected;
        }
    }
    return selected;
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
    if (report) {
        report->members_total = member_count;
        report->page_rule = page_rule == PDZ_PDF_PAGES_AUTO ? PDZ_PDF_PAGES_ALL : page_rule;
    }
    if (page_rule == PDZ_PDF_PAGES_AUTO) {
        page_rule = PDZ_PDF_PAGES_ALL;
    }
    for (index = 0u; index < member_count; ++index) {
        PdzBuffer payload = {0};
        if (page_rule != PDZ_PDF_PAGES_ALL &&
            ((page_rule == PDZ_PDF_PAGES_BODY && !member_is_body(&members[index])) ||
             (page_rule == PDZ_PDF_PAGES_BODY_ATT &&
              !member_is_body(&members[index]) && !member_is_attachment(&members[index])))) {
            continue;
        }
        rc = pdz_member_payload(data, size, &members[index], key_string, &payload);
        if (rc != PDG_OK) {
            if (report) ++report->skipped;
            continue;
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
