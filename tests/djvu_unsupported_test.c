#include "pdg_internal.h"
#include "djvu_parser.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
 * no JB2/IW44 image data. */
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

int main(void)
{
    uint8_t document[64];
    uint8_t page_pixels[3u * 4u];
    size_t document_size;
    size_t page_count = 0u;
    PdgDjvuReader *reader = NULL;
    PdgHeader header;
    PdgImage image;
    int rc;

    memset(&header, 0, sizeof(header));
    image.pixels = page_pixels;
    image.size = sizeof(page_pixels);
    image.width = 1u;
    image.height = 1u;
    image.stride = 3u;
    image.format = PDG_PIXEL_RGB24;

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
    CHECK(rc == PDG_ERR_UNSUPPORTED);
    CHECK(image.pixels == NULL && image.size == 0u);
    CHECK(pdg_djvu_reader_render_page(reader, 1u, &header, &image) ==
          PDG_ERR_INVALID_ARGUMENT);
    pdg_djvu_reader_close(reader);

    memset(&image, 0, sizeof(image));
    rc = pdg_render_djvu_payload(document, document_size, &header, &image);
    CHECK(rc == PDG_ERR_UNSUPPORTED);
    CHECK(image.pixels == NULL && image.size == 0u);

    /* Corrupt container must be rejected by the structure parser, not by a
     * pixel decoder. */
    memcpy(document + document_size - 1u, "\xff", 1u);
    write_be32(document + 8u, 0x7fffffffu);
    CHECK(qw_pdg_djvu_parse_page_count(document, document_size, &page_count) ==
          PDG_ERR_FORMAT);
    return 0;
}
