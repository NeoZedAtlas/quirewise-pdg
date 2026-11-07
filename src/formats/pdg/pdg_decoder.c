#include "pdg_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_file(const char *path, uint8_t **data_out, size_t *size_out)
{
    if (!path || !data_out || !size_out) {
        return PDG_ERR_INVALID_ARGUMENT;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return PDG_ERR_IO;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return PDG_ERR_IO;
    }
    long end = ftell(fp);
    if (end < 0) {
        fclose(fp);
        return PDG_ERR_IO;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return PDG_ERR_IO;
    }

    size_t size = (size_t)end;
    uint8_t *data = (uint8_t *)malloc(size ? size : 1);
    if (!data) {
        fclose(fp);
        return PDG_ERR_OUT_OF_MEMORY;
    }
    if (size && fread(data, 1, size, fp) != size) {
        free(data);
        fclose(fp);
        return PDG_ERR_IO;
    }
    fclose(fp);

    *data_out = data;
    *size_out = size;
    return PDG_OK;
}

static bool is_group4_type(uint8_t type)
{
    return type == 0x02 || type == 0x03 || type == 0x10 || (type >= 0x11 && type <= 0x1c);
}

static bool is_group4_payload(const PdgBuffer *out)
{
    /* Native PDF members use PDG type 0x1a, which is also inside the broad
     * historical Group4 type range. Once payload detection found a container
     * format, its signature takes precedence over the PDG header type. */
    if (!out || out->kind != PDG_OUTPUT_RAW) {
        return false;
    }
    if (is_group4_type(out->header.type)) {
        return true;
    }
    return out->header.type == 0x00 && out->header.color_depth == 1 && out->kind == PDG_OUTPUT_RAW;
}

static bool is_pdg2pic_supported_type(uint8_t type)
{
    return type <= 0x05 || type == 0x28 || type == 0xaa || type == 0xab || type == 0xac ||
           (type >= 0x10 && type <= 0x1c);
}

static int copy_direct_image(const uint8_t *file_data, size_t file_size, PdgBuffer *out, PdgOutputKind kind)
{
    uint8_t *data = (uint8_t *)malloc(file_size ? file_size : 1);
    if (!data) {
        return PDG_ERR_OUT_OF_MEMORY;
    }
    if (file_size) {
        memcpy(data, file_data, file_size);
    }
    memset(out, 0, sizeof(*out));
    out->data = data;
    out->size = file_size;
    out->kind = kind;
    return PDG_OK;
}

static int decrypt_loaded_file(const uint8_t *file_data, size_t file_size, PdgBuffer *out)
{
    if (file_size == 0) {
        return PDG_ERR_EMPTY;
    }

    PdgOutputKind direct_kind = pdg_detect_payload_kind(file_data, file_size);
    if (direct_kind != PDG_OUTPUT_RAW && !(file_size >= 2 && file_data[0] == 'H' && file_data[1] == 'H')) {
        return copy_direct_image(file_data, file_size, out, direct_kind);
    }
    if (file_size < 2 || file_data[0] != 'H' || file_data[1] != 'H') {
        return PDG_ERR_NOT_PDG;
    }

    PdgHeader header;
    int rc = pdg_parse_header(file_data, file_size, &header);
    if (rc != PDG_OK) {
        return rc;
    }
    if (!is_pdg2pic_supported_type(header.type)) {
        return PDG_ERR_UNSUPPORTED;
    }

    uint8_t *payload = (uint8_t *)malloc(header.data_size ? header.data_size : 1);
    if (!payload) {
        return PDG_ERR_OUT_OF_MEMORY;
    }
    memcpy(payload, file_data + header.data_offset, header.data_size);

    size_t header_area_size = header.data_offset;
    uint8_t *header_area = (uint8_t *)malloc(header_area_size ? header_area_size : 1);
    if (!header_area) {
        free(payload);
        return PDG_ERR_OUT_OF_MEMORY;
    }
    memcpy(header_area, file_data, header_area_size);

    if (header.type == 0x10 || (header.type >= 0x11 && header.type <= 0x1c)) {
        rc = pdg_apply_lightweight_scramble(header_area, header_area_size, payload, header.data_size, header.type);
    } else if (header.type == 0xaa || header.type == 0xab || header.type == 0xac) {
        rc = pdg_apply_optional_layer(header_area, header_area_size, payload, header.data_size);
        if (rc == PDG_OK) {
            rc = pdg_apply_a_family_transform(header_area, header_area_size, payload, header.data_size, header.type);
        }
    } else if (header.type == 0x28) {
        rc = pdg_apply_optional_layer(header_area, header_area_size, payload, header.data_size);
        if (rc == PDG_OK) {
            rc = pdg_apply_type28_transform(header_area, header_area_size, payload, header.data_size);
        }
    } else {
        rc = pdg_decrypt_payload_in_place(payload, header.data_size, &header);
    }
    free(header_area);

    if (rc != PDG_OK) {
        free(payload);
        return rc;
    }

    out->data = payload;
    out->size = header.data_size;
    out->kind = pdg_detect_payload_kind(payload, header.data_size);
    out->header = header;
    return PDG_OK;
}

PDG_API int pdg_abi_version(void)
{
    return PDG_ABI_VERSION;
}

PDG_API PdgContext *pdg_context_create(void)
{
    return (PdgContext *)calloc(1, sizeof(PdgContext));
}

PDG_API void pdg_context_free(PdgContext *ctx)
{
    if (!ctx) {
        return;
    }
    free(ctx->fax_reference);
    free(ctx->fax_current);
    free(ctx);
}

PDG_API int pdg_decrypt_memory_ctx(PdgContext *ctx, const void *data, size_t size, PdgBuffer *out)
{
    if ((!data && size != 0) || !out) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    (void)ctx;
    memset(out, 0, sizeof(*out));

    int rc = decrypt_loaded_file((const uint8_t *)data, size, out);
    if (rc != PDG_OK) {
        memset(out, 0, sizeof(*out));
    }
    return rc;
}

PDG_API int pdg_decrypt_memory(const void *data, size_t size, PdgBuffer *out)
{
    return pdg_decrypt_memory_ctx(NULL, data, size, out);
}

PDG_API int pdg_decrypt_file(const char *path, PdgBuffer *out)
{
    if (!path || !out) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));

    uint8_t *file_data = NULL;
    size_t file_size = 0;
    int rc = read_file(path, &file_data, &file_size);
    if (rc != PDG_OK) {
        return rc;
    }

    rc = pdg_decrypt_memory(file_data, file_size, out);
    free(file_data);
    return rc;
}

static int decode_decrypted_buffer(PdgContext *ctx, PdgBuffer *out)
{
    if (out->kind == PDG_OUTPUT_PDF || !is_group4_payload(out)) {
        return PDG_OK;
    }

    uint8_t *bmp = NULL;
    size_t bmp_size = 0;
    int rc = qw_pdg_decode_g4_to_bmp_ctx(ctx, out->data, out->size, out->header.width, out->header.height, &bmp, &bmp_size);
    if (rc != PDG_OK) {
        pdg_buffer_free(out);
        return rc;
    }

    free(out->data);
    out->data = bmp;
    out->size = bmp_size;
    out->kind = PDG_OUTPUT_BMP;
    return PDG_OK;
}

PDG_API int pdg_decode_memory_ctx(PdgContext *ctx, const void *data, size_t size, PdgBuffer *out)
{
    int rc = pdg_decrypt_memory_ctx(ctx, data, size, out);
    if (rc != PDG_OK) {
        return rc;
    }
    return decode_decrypted_buffer(ctx, out);
}

PDG_API int pdg_decode_memory(const void *data, size_t size, PdgBuffer *out)
{
    return pdg_decode_memory_ctx(NULL, data, size, out);
}

PDG_API int pdg_decode_file(const char *path, PdgBuffer *out)
{
    if (!path || !out) {
        return PDG_ERR_INVALID_ARGUMENT;
    }

    int rc = pdg_decrypt_file(path, out);
    if (rc != PDG_OK) {
        return rc;
    }
    return decode_decrypted_buffer(NULL, out);
}

PDG_API void pdg_buffer_free(PdgBuffer *buffer)
{
    if (!buffer) {
        return;
    }
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

PDG_API const char *pdg_error_string(int code)
{
    switch (code) {
    case PDG_OK:
        return "ok";
    case PDG_ERR_INVALID_ARGUMENT:
        return "invalid argument";
    case PDG_ERR_OUT_OF_MEMORY:
        return "out of memory";
    case PDG_ERR_IO:
        return "i/o error";
    case PDG_ERR_FORMAT:
        return "invalid or unsupported PDG format";
    case PDG_ERR_UNSUPPORTED:
        return "unsupported PDG feature";
    case PDG_ERR_DECODE:
        return "decode error";
    case PDG_ERR_NOT_PDG:
        return "not a PDG file";
    case PDG_ERR_EMPTY:
        return "empty input";
    case PDG_ERR_EXTERNAL_DECODER_REQUIRED:
        return "statically linked PDF rasterizer is not included in this build";
    case PDG_ERR_RESOURCE_LIMIT:
        return "resource limit exceeded";
    default:
        return "unknown error";
    }
}

PDG_API const char *pdg_output_extension(PdgOutputKind kind)
{
    switch (kind) {
    case PDG_OUTPUT_BMP:
        return "bmp";
    case PDG_OUTPUT_JPEG:
        return "jpg";
    case PDG_OUTPUT_PNG:
        return "png";
    case PDG_OUTPUT_DJVU:
        return "djvu";
    case PDG_OUTPUT_TIFF:
        return "tif";
    case PDG_OUTPUT_PDF:
        return "pdf";
    case PDG_OUTPUT_JP2:
        return "jp2";
    case PDG_OUTPUT_RAW:
    default:
        return "bin";
    }
}

PDG_API const char *pdg_output_kind_name(PdgOutputKind kind)
{
    switch (kind) {
    case PDG_OUTPUT_BMP:
        return "BMP";
    case PDG_OUTPUT_JPEG:
        return "JPEG";
    case PDG_OUTPUT_PNG:
        return "PNG";
    case PDG_OUTPUT_DJVU:
        return "DjVu";
    case PDG_OUTPUT_TIFF:
        return "TIFF";
    case PDG_OUTPUT_PDF:
        return "PDF";
    case PDG_OUTPUT_JP2:
        return "JPEG 2000";
    case PDG_OUTPUT_RAW:
    default:
        return "raw";
    }
}

PDG_API int pdg_output_requires_external_decoder(PdgOutputKind kind)
{
    switch (kind) {
    case PDG_OUTPUT_PDF:
        return 1;
    case PDG_OUTPUT_JP2:
        return 1;
    case PDG_OUTPUT_RAW:
    case PDG_OUTPUT_BMP:
    case PDG_OUTPUT_JPEG:
    case PDG_OUTPUT_PNG:
    case PDG_OUTPUT_DJVU:
    case PDG_OUTPUT_TIFF:
    default:
        return 0;
    }
}
