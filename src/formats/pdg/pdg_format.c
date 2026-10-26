#include "pdg_internal.h"

#include <string.h>

static uint16_t decode_type10_dimension_x(uint16_t x_cipher, uint16_t y_cipher, const uint8_t user_key[4])
{
    (void)y_cipher;
    uint32_t key = qw_read_u32_le(user_key);
    uint32_t high = key >> 16;
    uint32_t d = (key - (high - 219u * (uint32_t)((2510300521ull * high >> 32) >> 7)) *
                         (high - 511u * (uint32_t)((2151686161ull * high >> 32) >> 8)) +
                  20718u) &
                 0xffffu;
    uint32_t c = high - 131u * (uint32_t)((1049152317ull * high >> 32) >> 5);
    uint32_t e = d * high;
    return (uint16_t)(c - e - d % 1019u + x_cipher);
}

static uint16_t decode_type10_dimension_y(uint16_t x_cipher, uint16_t y_cipher, const uint8_t user_key[4])
{
    (void)x_cipher;
    uint32_t key = qw_read_u32_le(user_key);
    uint32_t high = key >> 16;
    uint32_t d = (key - (high - 219u * (uint32_t)((2510300521ull * high >> 32) >> 7)) *
                         (high - 511u * (uint32_t)((2151686161ull * high >> 32) >> 8)) +
                  20718u) &
                 0xffffu;
    uint32_t c = high - 131u * (uint32_t)((1049152317ull * high >> 32) >> 5);
    uint32_t e = d * high;
    return (uint16_t)(d % 1019u - c - e + y_cipher);
}

int pdg_parse_header(const uint8_t *file_data, size_t file_size, PdgHeader *header)
{
    if (!file_data || !header) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    if (file_size < PDG_HEADER_SIZE || file_data[0] != 'H' || file_data[1] != 'H') {
        return PDG_ERR_FORMAT;
    }

    memset(header, 0, sizeof(*header));
    header->version = file_data[2];
    header->type = file_data[0x0f];
    header->width_cipher = qw_read_u16_le(file_data + 0x10);
    header->height_cipher = qw_read_u16_le(file_data + 0x12);
    header->color_depth = file_data[0x14];
    header->layer_count = file_data[0x15];
    header->data_offset = qw_read_u32_le(file_data + 0x18);
    header->data_size = qw_read_u32_le(file_data + 0x1c);
    memcpy(header->key_data, file_data + 0x40, sizeof(header->key_data));
    memcpy(header->ss_user_key, file_data + 0x68, sizeof(header->ss_user_key));
    pdg_decode_dimensions(header);

    if (header->data_offset == 0 || header->data_offset > file_size || header->data_size > file_size - header->data_offset) {
        return PDG_ERR_FORMAT;
    }
    return PDG_OK;
}

void pdg_decode_dimensions(PdgHeader *header)
{
    if (!header) {
        return;
    }
    header->width = header->width_cipher;
    header->height = header->height_cipher;

    switch (header->type) {
    case 0xaa:
    case 0xac:
        header->width = 1120;
        header->height = 1568;
        break;
    case 0xab:
        header->width = header->height_cipher;
        header->height = header->width_cipher;
        break;
    case 0x10:
        header->width = decode_type10_dimension_x(header->width_cipher, header->height_cipher, header->ss_user_key);
        header->height = decode_type10_dimension_y(header->width_cipher, header->height_cipher, header->ss_user_key);
        break;
    default:
        break;
    }
}

PdgOutputKind pdg_detect_payload_kind(const uint8_t *data, size_t size)
{
    if (!data || size == 0) {
        return PDG_OUTPUT_RAW;
    }
    if (size >= 2 && data[0] == 0xff && data[1] == 0xd8) {
        return PDG_OUTPUT_JPEG;
    }
    if (size >= 2 && data[0] == 'B' && data[1] == 'M') {
        return PDG_OUTPUT_BMP;
    }
    if (size >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) {
        return PDG_OUTPUT_PNG;
    }
    if (size >= 4 && memcmp(data, "AT&T", 4) == 0) {
        return PDG_OUTPUT_DJVU;
    }
    if (size >= 2 && ((data[0] == 'I' && data[1] == 'I') || (data[0] == 'M' && data[1] == 'M'))) {
        return PDG_OUTPUT_TIFF;
    }
    if (size >= 4 && memcmp(data, "%PDF", 4) == 0) {
        return PDG_OUTPUT_PDF;
    }
    if (size >= 8 && memcmp(data, "\0\0\0\x0cjP  ", 8) == 0) {
        return PDG_OUTPUT_JP2;
    }
    if (size >= 4 && data[0] == 0xff && data[1] == 0x4f &&
        data[2] == 0xff && data[3] == 0x51) {
        return PDG_OUTPUT_JP2;
    }
    return PDG_OUTPUT_RAW;
}
