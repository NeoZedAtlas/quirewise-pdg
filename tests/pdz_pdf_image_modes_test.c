/* Regression coverage for the image dictionaries the PDG/PDZ PDF writer
 * emits. The JPEG path is the lossless one that a real SSReader conversion
 * uses, and it used to declare /DeviceRGB for single-component grey frames,
 * which made every conforming reader decode three samples per pixel and lose
 * the page body. */

#include <pdz_pdf.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pdz_pdf_image_fixtures.h"

static int g_failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

typedef struct MemSink {
    uint8_t *data;
    size_t size;
    size_t capacity;
    size_t calls;
    int fail_after; /* -1 = never fail, otherwise fail from this call on */
} MemSink;

static int mem_sink_write(void *context, const void *data, size_t size)
{
    MemSink *sink = (MemSink *)context;
    uint8_t *grown;
    if (!sink || !data) {
        return -1;
    }
    if (sink->fail_after >= 0 && (int)sink->calls >= sink->fail_after) {
        return -1;
    }
    if (sink->size + size > sink->capacity) {
        size_t capacity = sink->capacity ? sink->capacity : 4096u;
        while (capacity < sink->size + size) {
            capacity *= 2u;
        }
        grown = (uint8_t *)realloc(sink->data, capacity);
        if (!grown) {
            return -1;
        }
        sink->data = grown;
        sink->capacity = capacity;
    }
    memcpy(sink->data + sink->size, data, size);
    sink->size += size;
    sink->calls += 1u;
    return 0;
}

static int pdf_contains(const uint8_t *pdf, size_t size, const char *needle)
{
    size_t length = strlen(needle);
    size_t offset;
    if (!pdf || !length || size < length) {
        return 0;
    }
    for (offset = 0u; offset + length <= size; ++offset) {
        if (memcmp(pdf + offset, needle, length) == 0) {
            return 1;
        }
    }
    return 0;
}

static long pdf_startxref(const uint8_t *pdf, size_t size)
{
    static const char marker[] = "startxref\n";
    size_t length = sizeof(marker) - 1u;
    size_t offset;
    long value = 0;
    for (offset = 0u; offset + length <= size; ++offset) {
        if (memcmp(pdf + offset, marker, length) == 0) {
            offset += length;
            while (offset < size && pdf[offset] >= '0' && pdf[offset] <= '9') {
                value = value * 10 + (long)(pdf[offset] - '0');
                ++offset;
            }
            return value;
        }
    }
    return -1;
}

static void test_jpeg_color_spaces(void)
{
    uint8_t *pdf = NULL;
    size_t size = 0u;
    size_t pages = 0u;
    PdzPdfImage image;

    memset(&image, 0, sizeof(image));
    image.data = kGrayJpeg;
    image.size = sizeof(kGrayJpeg);
    image.kind = QW_PAGE_IMAGE_JPEG;
    image.width = 8u;
    image.height = 8u;
    CHECK(pdz_pdf_from_images(&image, 1u, PDZ_PDF_PAGE_A4, &pdf, &size, &pages) == PDG_OK);
    CHECK(pages == 1u && size > 128u);
    CHECK(pdf_contains(pdf, size, "/ColorSpace /DeviceGray"));
    CHECK(!pdf_contains(pdf, size, "/ColorSpace /DeviceRGB"));
    CHECK(!pdf_contains(pdf, size, "/Decode [1 0 1 0 1 0 1 0]"));
    CHECK(pdf_startxref(pdf, size) > 0);
    {
        long xref = pdf_startxref(pdf, size);
        CHECK(xref > 0 && (size_t)xref + 4u <= size &&
              memcmp(pdf + xref, "xref", 4u) == 0);
    }
    pdz_pdf_free(pdf);

    pdf = NULL;
    size = 0u;
    image.data = kRgbJpeg;
    image.size = sizeof(kRgbJpeg);
    CHECK(pdz_pdf_from_images(&image, 1u, PDZ_PDF_PAGE_A4, &pdf, &size, &pages) == PDG_OK);
    CHECK(pdf_contains(pdf, size, "/ColorSpace /DeviceRGB"));
    CHECK(!pdf_contains(pdf, size, "/ColorSpace /DeviceGray"));
    pdz_pdf_free(pdf);

    pdf = NULL;
    size = 0u;
    image.data = kCmykJpeg;
    image.size = sizeof(kCmykJpeg);
    CHECK(pdz_pdf_from_images(&image, 1u, PDZ_PDF_PAGE_A4, &pdf, &size, &pages) == PDG_OK);
    CHECK(pdf_contains(pdf, size, "/ColorSpace /DeviceCMYK"));
    CHECK(pdf_contains(pdf, size, "/Decode [1 0 1 0 1 0 1 0]"));
    pdz_pdf_free(pdf);
}

static void test_jpeg_frame_dimensions_win(void)
{
    uint8_t *pdf = NULL;
    size_t size = 0u;
    size_t pages = 0u;
    PdzPdfImage image;

    memset(&image, 0, sizeof(image));
    image.data = kGrayJpeg;
    image.size = sizeof(kGrayJpeg);
    image.kind = QW_PAGE_IMAGE_JPEG;
    image.width = 99u;  /* deliberately wrong: the frame is 8x8 */
    image.height = 77u;
    CHECK(pdz_pdf_from_images(&image, 1u, PDZ_PDF_PAGE_A4, &pdf, &size, &pages) == PDG_OK);
    CHECK(pdf_contains(pdf, size, "/Width 8 /Height 8"));
    pdz_pdf_free(pdf);
}

static void test_other_image_kinds(void)
{
    static const unsigned char ccitt[] = {0x18u, 0x00u, 0x10u, 0x01u};
    uint8_t gray[8u * 8u];
    uint8_t rgb[8u * 8u * 3u];
    uint8_t *pdf = NULL;
    size_t size = 0u;
    size_t pages = 0u;
    PdzPdfImage image;

    memset(gray, 0x80, sizeof(gray));
    memset(rgb, 0x40, sizeof(rgb));

    memset(&image, 0, sizeof(image));
    image.data = ccitt;
    image.size = sizeof(ccitt);
    image.kind = QW_PAGE_IMAGE_CCITT;
    image.width = 8u;
    image.height = 8u;
    CHECK(pdz_pdf_from_images(&image, 1u, PDZ_PDF_PAGE_A4, &pdf, &size, &pages) == PDG_OK);
    CHECK(pdf_contains(pdf, size, "/Filter /CCITTFaxDecode"));
    CHECK(pdf_contains(pdf, size, "/DecodeParms << /K -1 /Columns 8 /Rows 8 /BlackIs1 true >>"));
    pdz_pdf_free(pdf);

    pdf = NULL;
    size = 0u;
    image.data = gray;
    image.size = sizeof(gray);
    image.kind = QW_PAGE_IMAGE_GRAY8;
    CHECK(pdz_pdf_from_images(&image, 1u, PDZ_PDF_PAGE_A4, &pdf, &size, &pages) == PDG_OK);
    CHECK(pdf_contains(pdf, size, "/ColorSpace /DeviceGray /BitsPerComponent 8 /Length 64"));
    pdz_pdf_free(pdf);

    pdf = NULL;
    size = 0u;
    image.data = rgb;
    image.size = sizeof(rgb);
    image.kind = QW_PAGE_IMAGE_RGB24;
    CHECK(pdz_pdf_from_images(&image, 1u, PDZ_PDF_PAGE_A4, &pdf, &size, &pages) == PDG_OK);
    CHECK(pdf_contains(pdf, size, "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Length 192"));
    pdz_pdf_free(pdf);
}

static void test_streaming_writer(void)
{
    MemSink sink;
    PdzPdfWriter *writer = NULL;
    PdzPdfImage gray;
    PdzPdfImage color;
    uint8_t *reference = NULL;
    size_t reference_size = 0u;
    size_t pages = 0u;

    memset(&sink, 0, sizeof(sink));
    sink.fail_after = -1;
    memset(&gray, 0, sizeof(gray));
    gray.data = kGrayJpeg;
    gray.size = sizeof(kGrayJpeg);
    gray.kind = QW_PAGE_IMAGE_JPEG;
    gray.width = 8u;
    gray.height = 8u;
    color = gray;
    color.data = kRgbJpeg;
    color.size = sizeof(kRgbJpeg);

    CHECK(pdz_pdf_writer_create(PDZ_PDF_PAGE_A4, mem_sink_write, &sink, &writer) == PDG_OK);
    CHECK(writer != NULL);
    CHECK(pdz_pdf_writer_add_page(writer, &gray) == PDG_OK);
    CHECK(pdz_pdf_writer_add_page(writer, &color) == PDG_OK);
    CHECK(pdz_pdf_writer_finish(writer, &pages) == PDG_OK);
    CHECK(pages == 2u);
    CHECK(sink.calls >= 1u);
    CHECK(sink.size > 256u);
    CHECK(memcmp(sink.data, "%PDF-1.4", 8u) == 0);
    CHECK(pdf_contains(sink.data, sink.size, "/Count 2"));
    CHECK(pdf_contains(sink.data, sink.size, "3 0 R 6 0 R"));
    {
        long xref = pdf_startxref(sink.data, sink.size);
        CHECK(xref > 0 && (size_t)xref + 4u <= sink.size &&
              memcmp(sink.data + xref, "xref", 4u) == 0);
        CHECK(pdf_contains(sink.data, sink.size, "trailer\n<< /Size 9 /Root 1 0 R >>"));
    }
    pdz_pdf_writer_destroy(writer);

    /* The streaming path and the in-memory path must produce identical bytes. */
    {
        PdzPdfImage pair[2];
        pair[0] = gray;
        pair[1] = color;
        CHECK(pdz_pdf_from_images(pair, 2u, PDZ_PDF_PAGE_A4, &reference,
                                  &reference_size, &pages) == PDG_OK);
        CHECK(reference_size == sink.size);
        CHECK(reference_size == 0u ||
              memcmp(reference, sink.data, reference_size) == 0);
        pdz_pdf_free(reference);
    }
    free(sink.data);
}

static void test_writer_errors(void)
{
    MemSink sink;
    PdzPdfWriter *writer = NULL;
    PdzPdfImage gray;
    size_t pages = 99u;
    int failed = 0;
    int rc;

    memset(&gray, 0, sizeof(gray));
    gray.data = kGrayJpeg;
    gray.size = sizeof(kGrayJpeg);
    gray.kind = QW_PAGE_IMAGE_JPEG;
    gray.width = 8u;
    gray.height = 8u;

    /* A sink that refuses every byte must fail before a document is claimed. */
    memset(&sink, 0, sizeof(sink));
    sink.fail_after = 0;
    rc = pdz_pdf_writer_create(PDZ_PDF_PAGE_A4, mem_sink_write, &sink, &writer);
    if (rc != PDG_OK) {
        failed = 1;
    } else {
        if (pdz_pdf_writer_add_page(writer, &gray) != PDG_OK) {
            failed = 1;
        } else if (pdz_pdf_writer_finish(writer, &pages) != PDG_OK) {
            failed = 1;
        }
    }
    CHECK(failed);
    pdz_pdf_writer_destroy(writer);
    free(sink.data);

    /* A writer with no pages must not publish an empty page tree. */
    memset(&sink, 0, sizeof(sink));
    sink.fail_after = -1;
    CHECK(pdz_pdf_writer_create(PDZ_PDF_PAGE_IMAGE, mem_sink_write, &sink, &writer) == PDG_OK);
    CHECK(pdz_pdf_writer_finish(writer, &pages) == PDG_ERR_EMPTY);
    CHECK(pages == 0u);
    pdz_pdf_writer_destroy(writer);
    free(sink.data);

    pdz_pdf_writer_destroy(NULL);
    CHECK(pdz_pdf_writer_create(PDZ_PDF_PAGE_A4, NULL, NULL, &writer) == PDG_ERR_INVALID_ARGUMENT);
    CHECK(pdz_pdf_writer_create(99, mem_sink_write, NULL, &writer) == PDG_ERR_INVALID_ARGUMENT);
}

int main(void)
{
    test_jpeg_color_spaces();
    test_jpeg_frame_dimensions_win();
    test_other_image_kinds();
    test_streaming_writer();
    test_writer_errors();
    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    puts("PASS: PDF image dictionaries and streaming writer");
    return 0;
}


