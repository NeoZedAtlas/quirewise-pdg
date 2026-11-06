#include "pdg_internal.h"
#include "djvu_parser.h"
#include "quirewise/djvu.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_PDG_DJVU_FIXTURE
#define QUIREWISE_PDG_DJVU_FIXTURE \
    "../quirewise-page/tests/data/djvu/bitonal.djvu"
#endif

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static void write_be32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24);
    output[1] = (uint8_t)(value >> 16);
    output[2] = (uint8_t)(value >> 8);
    output[3] = (uint8_t)value;
}

/* Minimal single-page DJVU FORM: AT&TFORM + form length + "DJVU" + one INFO
 * chunk (10 bytes) + a small payload chunk. Structure-only fixture; it carries
 * no valid JB2 image data, so rendering must fail cleanly. */
static size_t build_single_page_djvu(uint8_t *output, size_t capacity)
{
    static const uint8_t info_payload[10] = {
        0u, 0u, 0u, 32u, 0u, 0u, 0u, 16u, 1u, 24u
    };
    static const uint8_t sjbz_payload[4] = {0x01u, 0x02u, 0x03u, 0x04u};
    size_t position = 0u;
    size_t form_length;
    size_t form_data_length;
    if (!output || capacity < 64u) return 0u;
    form_data_length = 4u + 8u + sizeof(info_payload) +
                       8u + sizeof(sjbz_payload);
    form_length = (uint32_t)form_data_length;
    if (12u + form_data_length > capacity) return 0u;
    memcpy(output + position, "AT&TFORM", 8u);
    position += 8u;
    write_be32(output + position, (uint32_t)form_length);
    position += 4u;
    memcpy(output + position, "DJVU", 4u);
    position += 4u;
    memcpy(output + position, "INFO", 4u);
    position += 4u;
    write_be32(output + position, (uint32_t)sizeof(info_payload));
    position += 4u;
    memcpy(output + position, info_payload, sizeof(info_payload));
    position += sizeof(info_payload);
    memcpy(output + position, "Sjbz", 4u);
    position += 4u;
    write_be32(output + position, (uint32_t)sizeof(sjbz_payload));
    position += 4u;
    memcpy(output + position, sjbz_payload, sizeof(sjbz_payload));
    position += sizeof(sjbz_payload);
    return position;
}

static uint8_t *read_file(const char *path, size_t *out_size)
{
    FILE *file = fopen(path, "rb");
    long length;
    uint8_t *data;
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (length <= 0) { fclose(file); return NULL; }
    data = (uint8_t *)malloc((size_t)length);
    if (!data) { fclose(file); return NULL; }
    if (fread(data, 1u, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *out_size = (size_t)length;
    return data;
}

static int test_real_fixture(void)
{
    size_t size = 0u, pdg_pages = 0u, page_pages = 0u, text_size = 0u;
    uint8_t *data = read_file(QUIREWISE_PDG_DJVU_FIXTURE, &size);
    PdgDjvuReader *reader = NULL;
    PdgHeader header;
    PdgImage image;
    QwDjvuImage page_image;
    QwDjvuPageInfo info;
    char *text = NULL;
    CHECK(data != NULL);
    memset(&header, 0, sizeof(header));
    CHECK(qw_pdg_djvu_probe(data, size) == 1);
    CHECK(qw_pdg_djvu_parse_page_count(data, size, &pdg_pages) == PDG_OK);
    CHECK(qw_djvu_get_page_count(data, size, &page_pages) == QW_DJVU_OK);
    CHECK(pdg_pages == page_pages && pdg_pages == 1u);
    CHECK(qw_djvu_get_page_info(data, size, 0u, &info) == QW_DJVU_OK);
    CHECK(pdg_djvu_reader_open(data, size, &reader, &pdg_pages) == PDG_OK);
    memset(&image, 0, sizeof(image));
    CHECK(pdg_djvu_reader_render_page(reader, 0u, &header, &image) == PDG_OK);
    CHECK(image.width == info.width && image.height == info.height);
    CHECK(image.format == PDG_PIXEL_GRAY8);
    CHECK(image.pixels != NULL && image.size == info.width * info.height);
    CHECK(qw_djvu_render_page(data, size, 0u, 1u, &page_image) == QW_DJVU_OK);
    CHECK(page_image.size == image.size);
    CHECK(memcmp(page_image.pixels, image.pixels, image.size) == 0);
    qw_djvu_image_free(&page_image);
    pdg_image_free(&image);
    CHECK(qw_djvu_extract_text(data, size, 0u, &text, &text_size) == QW_DJVU_OK);
    qw_djvu_text_free(text);
    CHECK(pdg_djvu_reader_render_page(reader, 1u, &header, &image) ==
          PDG_ERR_INVALID_ARGUMENT);
    pdg_djvu_reader_close(reader);
    free(data);
    return 0;
}

int main(void)
{
    uint8_t document[64];
    size_t document_size;
    size_t page_count = 0u;
    PdgDjvuReader *reader = NULL;
    PdgHeader header;
    PdgImage image;
    int rc;

    memset(&header, 0, sizeof(header));

    document_size = build_single_page_djvu(document, sizeof(document));
    CHECK(document_size > 0u);
    CHECK(qw_pdg_djvu_probe(document, document_size) == 1);
    CHECK(qw_pdg_djvu_parse_page_count(document, document_size, &page_count) == PDG_OK);
    CHECK(page_count == 1u);

    rc = pdg_djvu_reader_open(document, document_size, &reader, &page_count);
    CHECK(rc == PDG_OK);
    CHECK(reader != NULL);
    CHECK(page_count == 1u);

    memset(&image, 0, sizeof(image));
    rc = pdg_djvu_reader_render_page(reader, 0u, &header, &image);
    CHECK(rc != PDG_OK);
    CHECK(image.pixels == NULL && image.size == 0u);
    CHECK(pdg_djvu_reader_render_page(reader, 1u, &header, &image) ==
          PDG_ERR_INVALID_ARGUMENT);
    pdg_djvu_reader_close(reader);

    memset(&image, 0, sizeof(image));
    rc = pdg_render_djvu_payload(document, document_size, &header, &image);
    CHECK(rc != PDG_OK);
    CHECK(image.pixels == NULL && image.size == 0u);

    /* Corrupt container must be rejected by the structure parser, not by a
     * pixel decoder. */
    memcpy(document + document_size - 1u, "\xff", 1u);
    write_be32(document + 8u, 0x7fffffffu);
    CHECK(qw_pdg_djvu_parse_page_count(document, document_size, &page_count) ==
          PDG_ERR_FORMAT);

    CHECK(test_real_fixture() == 0);
    return 0;
}
