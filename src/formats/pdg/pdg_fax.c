#include "pdg_internal.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

enum {
    FAX_NOP = 0,
    FAX_PASS = 1,
    FAX_HORIZONTAL = 2,
    FAX_V0 = 3,
    FAX_VR1 = 4,
    FAX_VR2 = 5,
    FAX_VR3 = 6,
    FAX_VL1 = 7,
    FAX_VL2 = 8,
    FAX_VL3 = 9
};

typedef struct FaxBitReader {
    const uint8_t *data;
    size_t size;
    size_t byte_pos;
    uint64_t bit_buffer;
    int bit_count;
} FaxBitReader;

typedef struct FaxDecodeEntry {
    int value;
    uint8_t length;
} FaxDecodeEntry;

typedef struct FaxDecodeTables {
    FaxDecodeEntry white[65536];
    FaxDecodeEntry black[65536];
    FaxDecodeEntry mode[65536];
} FaxDecodeTables;

typedef int (*FaxRowWriter)(void *ctx, int y, const int *changes, size_t change_count);

typedef struct BitsRowWriter {
    uint8_t *bits;
    size_t stride;
    int width;
} BitsRowWriter;

typedef struct BmpRowWriter {
    uint8_t *pixels;
    size_t stride;
    int width;
    int height;
} BmpRowWriter;

typedef struct Gray8RowWriter {
    uint8_t *pixels;
    size_t stride;
    int width;
} Gray8RowWriter;

static FaxDecodeTables g_fax_decode_tables;
static atomic_int g_fax_decode_tables_state;

static void build_decode_table(FaxDecodeEntry *lookup, const PdgPrefixCode *codes, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        const uint8_t length = codes[i].length;
        if (length == 0 || length > 16) {
            continue;
        }
        const uint32_t prefix = (uint32_t)codes[i].bits << (16u - length);
        const uint32_t fill_count = 1u << (16u - length);
        for (uint32_t j = 0; j < fill_count; ++j) {
            lookup[prefix | j].value = codes[i].value;
            lookup[prefix | j].length = length;
        }
    }
}

static void ensure_fax_decode_tables(void)
{
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&g_fax_decode_tables_state,
                                                &expected,
                                                1,
                                                memory_order_acquire,
                                                memory_order_relaxed)) {
        build_decode_table(g_fax_decode_tables.white, pdg_fax_white_codes, pdg_fax_white_code_count);
        build_decode_table(g_fax_decode_tables.black, pdg_fax_black_codes, pdg_fax_black_code_count);
        build_decode_table(g_fax_decode_tables.mode, pdg_fax_mode_codes, pdg_fax_mode_code_count);
        atomic_store_explicit(&g_fax_decode_tables_state, 2, memory_order_release);
        return;
    }

    while (atomic_load_explicit(&g_fax_decode_tables_state, memory_order_acquire) != 2) {
    }
}

static void fax_ensure_bits(FaxBitReader *br, int bit_count)
{
    while (br->bit_count < bit_count) {
        uint8_t byte = 0xff;
        if (br->byte_pos < br->size) {
            byte = br->data[br->byte_pos];
        }
        br->byte_pos += 1;
        br->bit_buffer = (br->bit_buffer << 8) | byte;
        br->bit_count += 8;
    }
}

static uint32_t fax_peek_bits(FaxBitReader *br, int bit_count)
{
    fax_ensure_bits(br, bit_count);
    return (uint32_t)((br->bit_buffer >> (br->bit_count - bit_count)) & ((1u << bit_count) - 1u));
}

static void fax_skip_bits(FaxBitReader *br, int bit_count)
{
    br->bit_count -= bit_count;
    if (br->bit_count == 0) {
        br->bit_buffer = 0;
    } else {
        br->bit_buffer &= (1ull << br->bit_count) - 1ull;
    }
}

static int fax_read_prefix_fast(FaxBitReader *br, const FaxDecodeEntry *lookup, int *value_out)
{
    const uint32_t key = fax_peek_bits(br, 16);
    const FaxDecodeEntry entry = lookup[key];
    if (entry.length == 0) {
        return PDG_ERR_DECODE;
    }
    fax_skip_bits(br, entry.length);
    *value_out = entry.value;
    return PDG_OK;
}

static int fax_read_run(FaxBitReader *br, bool black, int *run_out)
{
    const FaxDecodeEntry *lookup = black ? g_fax_decode_tables.black : g_fax_decode_tables.white;
    int total = 0;

    for (int i = 0; i < 1024; ++i) {
        int value = 0;
        int rc = fax_read_prefix_fast(br, lookup, &value);
        if (rc != PDG_OK) {
            return rc;
        }
        total += value;
        if (value < 64) {
            *run_out = total;
            return PDG_OK;
        }
    }
    return PDG_ERR_DECODE;
}

static int clamp_int(int value, int lo, int hi)
{
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

static int append_change(int *changes, size_t capacity, size_t *count, int value)
{
    if (*count >= capacity) {
        return PDG_ERR_DECODE;
    }
    changes[*count] = value;
    *count += 1;
    return PDG_OK;
}

static void paint_line(uint8_t *row, int width, const int *changes, size_t change_count)
{
    for (size_t i = 0; i + 1 < change_count; i += 2) {
        int start = clamp_int(changes[i], 0, width);
        int end = clamp_int(changes[i + 1], start, width);
        if (start >= end) {
            continue;
        }

        while (start < end && (start & 7) != 0) {
            row[start >> 3] |= (uint8_t)(0x80u >> (start & 7));
            ++start;
        }

        size_t byte_pos = (size_t)start >> 3;
        size_t full_bytes = (size_t)(end - start) >> 3;
        if (full_bytes) {
            memset(row + byte_pos, 0xff, full_bytes);
            start += (int)(full_bytes << 3);
        }

        while (start < end) {
            row[start >> 3] |= (uint8_t)(0x80u >> (start & 7));
            ++start;
        }
    }
}

static int vertical_delta(int mode, int *delta_out)
{
    switch (mode) {
    case FAX_V0:
        *delta_out = 0;
        return PDG_OK;
    case FAX_VR1:
        *delta_out = 1;
        return PDG_OK;
    case FAX_VR2:
        *delta_out = 2;
        return PDG_OK;
    case FAX_VR3:
        *delta_out = 3;
        return PDG_OK;
    case FAX_VL1:
        *delta_out = -1;
        return PDG_OK;
    case FAX_VL2:
        *delta_out = -2;
        return PDG_OK;
    case FAX_VL3:
        *delta_out = -3;
        return PDG_OK;
    default:
        return PDG_ERR_DECODE;
    }
}

static int write_bits_row(void *ctx, int y, const int *changes, size_t change_count)
{
    BitsRowWriter *writer = (BitsRowWriter *)ctx;
    paint_line(writer->bits + (size_t)y * writer->stride, writer->width, changes, change_count);
    return PDG_OK;
}

static int write_bmp_row(void *ctx, int y, const int *changes, size_t change_count)
{
    BmpRowWriter *writer = (BmpRowWriter *)ctx;
    uint8_t *row = writer->pixels + (size_t)(writer->height - 1 - y) * writer->stride;
    paint_line(row, writer->width, changes, change_count);
    return PDG_OK;
}

static int write_gray8_row(void *ctx, int y, const int *changes, size_t change_count)
{
    Gray8RowWriter *writer = (Gray8RowWriter *)ctx;
    uint8_t *row = writer->pixels + (size_t)y * writer->stride;
    for (size_t i = 0; i + 1 < change_count; i += 2) {
        int start = clamp_int(changes[i], 0, writer->width);
        int end = clamp_int(changes[i + 1], start, writer->width);
        if (start < end) {
            memset(row + start, 0x00, (size_t)(end - start));
        }
    }
    return PDG_OK;
}

static int acquire_change_buffers(PdgContext *ctx,
                                  size_t capacity,
                                  int **reference_out,
                                  int **current_out,
                                  bool *owned_out)
{
    if (!reference_out || !current_out || !owned_out) {
        return PDG_ERR_INVALID_ARGUMENT;
    }

    if (!ctx) {
        int *reference = (int *)malloc(capacity * sizeof(*reference));
        int *current = (int *)malloc(capacity * sizeof(*current));
        if (!reference || !current) {
            free(reference);
            free(current);
            return PDG_ERR_OUT_OF_MEMORY;
        }
        *reference_out = reference;
        *current_out = current;
        *owned_out = true;
        return PDG_OK;
    }

    if (ctx->fax_change_capacity < capacity) {
        int *reference = (int *)malloc(capacity * sizeof(*reference));
        int *current = (int *)malloc(capacity * sizeof(*current));
        if (!reference || !current) {
            free(reference);
            free(current);
            return PDG_ERR_OUT_OF_MEMORY;
        }
        free(ctx->fax_reference);
        free(ctx->fax_current);
        ctx->fax_reference = reference;
        ctx->fax_current = current;
        ctx->fax_change_capacity = capacity;
    }

    *reference_out = ctx->fax_reference;
    *current_out = ctx->fax_current;
    *owned_out = false;
    return PDG_OK;
}

static void release_change_buffers(bool owned, int *reference, int *current)
{
    if (owned) {
        free(reference);
        free(current);
    }
}

static int decode_g4_rows(PdgContext *ctx,
                          const uint8_t *data,
                          size_t size,
                          int width,
                          int height,
                          FaxRowWriter write_row,
                          void *write_ctx)
{
    ensure_fax_decode_tables();

    size_t change_capacity = (size_t)width + 8u;
    int *reference = NULL;
    int *current = NULL;
    bool owned_buffers = false;
    int rc = acquire_change_buffers(ctx, change_capacity, &reference, &current, &owned_buffers);
    if (rc != PDG_OK) {
        return rc;
    }

    reference[0] = 0;
    reference[1] = width;
    reference[2] = width;
    reference[3] = width;
    reference[4] = width;
    size_t reference_count = 5;

    FaxBitReader br;
    memset(&br, 0, sizeof(br));
    br.data = data;
    br.size = size;

    for (int y = 0; y < height; ++y) {
        int a0 = 0;
        int color_flag = 0;
        size_t ref_idx = 0;
        size_t current_count = 0;
        size_t op_limit = (size_t)width * 8u + 4096u;

        while (a0 < width) {
            if (op_limit-- == 0) {
                release_change_buffers(owned_buffers, reference, current);
                return PDG_ERR_DECODE;
            }

            int mode = 0;
            rc = fax_read_prefix_fast(&br, g_fax_decode_tables.mode, &mode);
            if (rc != PDG_OK) {
                release_change_buffers(owned_buffers, reference, current);
                return rc;
            }

            if (mode == FAX_NOP) {
                continue;
            }

            if (mode == FAX_PASS) {
                a0 = (ref_idx + 1 < reference_count) ? reference[ref_idx + 1] : width;
                ref_idx += 2;
                continue;
            }

            if (mode == FAX_HORIZONTAL) {
                int run1 = 0;
                int run2 = 0;
                bool first_run_is_black = color_flag == 0;
                rc = fax_read_run(&br, first_run_is_black, &run1);
                if (rc == PDG_OK) {
                    rc = append_change(current, change_capacity, &current_count, clamp_int(a0 + run1, 0, width));
                }
                if (rc == PDG_OK) {
                    rc = fax_read_run(&br, !first_run_is_black, &run2);
                }
                if (rc == PDG_OK) {
                    rc = append_change(current, change_capacity, &current_count, clamp_int(a0 + run1 + run2, 0, width));
                }
                if (rc != PDG_OK) {
                    release_change_buffers(owned_buffers, reference, current);
                    return rc;
                }
                a0 += run1 + run2;
                while (ref_idx < reference_count && reference[ref_idx] <= a0) {
                    ref_idx += 2;
                }
                continue;
            }

            int delta = 0;
            rc = vertical_delta(mode, &delta);
            if (rc != PDG_OK) {
                release_change_buffers(owned_buffers, reference, current);
                return rc;
            }
            int b1 = (ref_idx < reference_count) ? reference[ref_idx] : width;
            int a1 = clamp_int(b1 + delta, 0, width);
            rc = append_change(current, change_capacity, &current_count, a1);
            if (rc != PDG_OK) {
                release_change_buffers(owned_buffers, reference, current);
                return rc;
            }
            a0 = a1;
            ref_idx += 1;
            color_flag = color_flag ? 0 : 1;
        }

        if (current_count == 0 || current[current_count - 1] != width) {
            int rc = append_change(current, change_capacity, &current_count, width);
            if (rc != PDG_OK) {
                release_change_buffers(owned_buffers, reference, current);
                return rc;
            }
        }

        rc = write_row(write_ctx, y, current, current_count);
        if (rc != PDG_OK) {
            release_change_buffers(owned_buffers, reference, current);
            return rc;
        }

        int *tmp = reference;
        reference = current;
        current = tmp;
        reference_count = current_count;
        for (size_t i = 0; i < 4 && reference_count < change_capacity; ++i) {
            reference[reference_count++] = width;
        }
    }

    release_change_buffers(owned_buffers, reference, current);
    return PDG_OK;
}

int pdg_decode_g4(const uint8_t *data, size_t size, int width, int height, uint8_t **bits_out, size_t *bits_size_out)
{
    if (!data || !bits_out || !bits_size_out || width <= 0 || height <= 0) {
        return PDG_ERR_INVALID_ARGUMENT;
    }

    size_t stride = ((size_t)width + 7u) / 8u;
    size_t image_size = stride * (size_t)height;
    if (stride == 0 || image_size / stride != (size_t)height) {
        return PDG_ERR_FORMAT;
    }

    uint8_t *bits = (uint8_t *)calloc(image_size, 1);
    if (!bits) {
        return PDG_ERR_OUT_OF_MEMORY;
    }

    BitsRowWriter writer = { bits, stride, width };
    int rc = decode_g4_rows(NULL, data, size, width, height, write_bits_row, &writer);
    if (rc != PDG_OK) {
        free(bits);
        return rc;
    }

    *bits_out = bits;
    *bits_size_out = image_size;
    return PDG_OK;
}

static void write_bmp_header(uint8_t *bmp, size_t file_size, size_t pixel_offset, size_t image_size, int width, int height)
{
    bmp[0] = 'B';
    bmp[1] = 'M';
    qw_write_u32_le(bmp + 2, (uint32_t)file_size);
    qw_write_u32_le(bmp + 10, (uint32_t)pixel_offset);
    qw_write_u32_le(bmp + 14, 40);
    qw_write_u32_le(bmp + 18, (uint32_t)width);
    qw_write_u32_le(bmp + 22, (uint32_t)height);
    qw_write_u16_le(bmp + 26, 1);
    qw_write_u16_le(bmp + 28, 1);
    qw_write_u32_le(bmp + 34, (uint32_t)image_size);
    qw_write_u32_le(bmp + 46, 2);

    bmp[54] = 0x00;
    bmp[55] = 0x00;
    bmp[56] = 0x00;
    bmp[57] = 0x00;
    bmp[58] = 0xff;
    bmp[59] = 0xff;
    bmp[60] = 0xff;
    bmp[61] = 0x00;
}

int pdg_bmp_from_1bpp(const uint8_t *bits, int width, int height, uint8_t **bmp_out, size_t *bmp_size_out)
{
    if (!bits || !bmp_out || !bmp_size_out || width <= 0 || height <= 0) {
        return PDG_ERR_INVALID_ARGUMENT;
    }

    size_t src_stride = ((size_t)width + 7u) / 8u;
    size_t dst_stride = ((size_t)width + 31u) / 32u * 4u;
    size_t pixel_offset = 14u + 40u + 8u;
    size_t image_size = dst_stride * (size_t)height;
    size_t file_size = pixel_offset + image_size;
    if (dst_stride == 0 || image_size / dst_stride != (size_t)height || file_size < pixel_offset) {
        return PDG_ERR_FORMAT;
    }

    uint8_t *bmp = (uint8_t *)calloc(file_size, 1);
    if (!bmp) {
        return PDG_ERR_OUT_OF_MEMORY;
    }

    write_bmp_header(bmp, file_size, pixel_offset, image_size, width, height);

    uint8_t *dst = bmp + pixel_offset;
    for (int y = height - 1; y >= 0; --y) {
        memcpy(dst, bits + (size_t)y * src_stride, src_stride);
        dst += dst_stride;
    }

    *bmp_out = bmp;
    *bmp_size_out = file_size;
    return PDG_OK;
}

int pdg_decode_g4_to_bmp(const uint8_t *data, size_t size, int width, int height, uint8_t **bmp_out, size_t *bmp_size_out)
{
    return pdg_decode_g4_to_bmp_ctx(NULL, data, size, width, height, bmp_out, bmp_size_out);
}

int pdg_decode_g4_to_bmp_ctx(PdgContext *ctx,
                             const uint8_t *data,
                             size_t size,
                             int width,
                             int height,
                             uint8_t **bmp_out,
                             size_t *bmp_size_out)
{
    if (!data || !bmp_out || !bmp_size_out || width <= 0 || height <= 0) {
        return PDG_ERR_INVALID_ARGUMENT;
    }

    size_t dst_stride = ((size_t)width + 31u) / 32u * 4u;
    size_t pixel_offset = 14u + 40u + 8u;
    size_t image_size = dst_stride * (size_t)height;
    size_t file_size = pixel_offset + image_size;
    if (dst_stride == 0 || image_size / dst_stride != (size_t)height || file_size < pixel_offset) {
        return PDG_ERR_FORMAT;
    }

    uint8_t *bmp = (uint8_t *)calloc(file_size, 1);
    if (!bmp) {
        return PDG_ERR_OUT_OF_MEMORY;
    }
    write_bmp_header(bmp, file_size, pixel_offset, image_size, width, height);

    BmpRowWriter writer = { bmp + pixel_offset, dst_stride, width, height };
    int rc = decode_g4_rows(ctx, data, size, width, height, write_bmp_row, &writer);
    if (rc != PDG_OK) {
        free(bmp);
        return rc;
    }

    *bmp_out = bmp;
    *bmp_size_out = file_size;
    return PDG_OK;
}

int pdg_decode_g4_to_gray8_ctx(PdgContext *ctx,
                               const uint8_t *data,
                               size_t size,
                               int width,
                               int height,
                               PdgImage *out)
{
    if (!data || !out || width <= 0 || height <= 0) {
        return PDG_ERR_INVALID_ARGUMENT;
    }

    size_t stride = (size_t)width;
    size_t image_size = stride * (size_t)height;
    if (stride == 0 || image_size / stride != (size_t)height) {
        return PDG_ERR_FORMAT;
    }

    uint8_t *pixels = (uint8_t *)malloc(image_size ? image_size : 1);
    if (!pixels) {
        return PDG_ERR_OUT_OF_MEMORY;
    }
    memset(pixels, 0xff, image_size);

    Gray8RowWriter writer = { pixels, stride, width };
    int rc = decode_g4_rows(ctx, data, size, width, height, write_gray8_row, &writer);
    if (rc != PDG_OK) {
        free(pixels);
        return rc;
    }

    memset(out, 0, sizeof(*out));
    out->pixels = pixels;
    out->size = image_size;
    out->width = (uint32_t)width;
    out->height = (uint32_t)height;
    out->stride = (uint32_t)stride;
    out->format = PDG_PIXEL_GRAY8;
    return PDG_OK;
}
