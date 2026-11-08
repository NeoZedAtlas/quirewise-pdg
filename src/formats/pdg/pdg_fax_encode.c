#include "pdg_internal.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* CCITT Group 4 (ITU-T T.6) encoder.
 *
 * Every line is coded as a sequence of horizontal-mode pairs, which is a valid
 * (if not size-optimal) T.6 stream: the decoder walks the same run list the
 * encoder produced, so no reference-line state can drift. The alternative,
 * mixing pass/vertical modes, would save roughly a third of the bytes but needs
 * a full a0/b1/b2 state machine on both sides. Books whose pages are stored as
 * a private Group 4 variant are the only callers, and a 3x overhead on an
 * already compact bitstream still beats the 8 bytes per pixel raw fallback by
 * two orders of magnitude.
 *
 * Bit conventions match the decoder: rows are packed MSB-first, one means
 * black, and the first run of every line is white. */

#define FAX_HORIZONTAL_CODE 0x1u
#define FAX_HORIZONTAL_BITS 3u
#define FAX_EOFB_BITS 24u
#define FAX_EOFB_VALUE 0x001001u

typedef struct FaxBitWriter {
    uint8_t *data;
    size_t bit_count;
    size_t capacity; /* in bytes */
} FaxBitWriter;

typedef struct FaxRunCodes {
    PdgPrefixCode codes[2561];
    int ready;
} FaxRunCodes;

static FaxRunCodes g_white_run_codes;
static FaxRunCodes g_black_run_codes;
static atomic_int g_run_codes_state;

static void build_run_codes(FaxRunCodes *table, const PdgPrefixCode *codes, size_t count)
{
    size_t index;
    memset(table->codes, 0, sizeof(table->codes));
    for (index = 0u; index < count; ++index) {
        int value = codes[index].value;
        if (value < 0 || value > 2560 || codes[index].length == 0u) {
            continue;
        }
        table->codes[value] = codes[index];
    }
    table->ready = 1;
}

static void ensure_run_codes(void)
{
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&g_run_codes_state, &expected, 1,
                                                memory_order_acquire,
                                                memory_order_relaxed)) {
        build_run_codes(&g_white_run_codes, pdg_fax_white_codes, pdg_fax_white_code_count);
        build_run_codes(&g_black_run_codes, pdg_fax_black_codes, pdg_fax_black_code_count);
        atomic_store_explicit(&g_run_codes_state, 2, memory_order_release);
        return;
    }
    while (atomic_load_explicit(&g_run_codes_state, memory_order_acquire) != 2) {
    }
}

static int writer_reserve(FaxBitWriter *writer, size_t extra_bits)
{
    size_t required = (writer->bit_count + extra_bits + 7u) / 8u;
    size_t capacity;
    uint8_t *grown;
    if (required <= writer->capacity) {
        return PDG_OK;
    }
    capacity = writer->capacity ? writer->capacity : 4096u;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2u) {
            capacity = required;
            break;
        }
        capacity *= 2u;
    }
    grown = (uint8_t *)realloc(writer->data, capacity);
    if (!grown) {
        return PDG_ERR_OUT_OF_MEMORY;
    }
    memset(grown + writer->capacity, 0, capacity - writer->capacity);
    writer->data = grown;
    writer->capacity = capacity;
    return PDG_OK;
}

static int writer_put_bits(FaxBitWriter *writer, uint32_t bits, unsigned length)
{
    unsigned index;
    int rc;
    if (!writer || length == 0u || length > 32u) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    rc = writer_reserve(writer, length);
    if (rc != PDG_OK) {
        return rc;
    }
    for (index = 0u; index < length; ++index) {
        unsigned bit = (bits >> (length - 1u - index)) & 1u;
        if (bit) {
            size_t position = writer->bit_count;
            writer->data[position >> 3] |= (uint8_t)(0x80u >> (position & 7u));
        }
        writer->bit_count += 1u;
    }
    return PDG_OK;
}

static int writer_put_run(FaxBitWriter *writer, int run, int black)
{
    const PdgPrefixCode *table = black ? g_black_run_codes.codes : g_white_run_codes.codes;
    int remaining = run;
    int rc;
    while (remaining > 2560) {
        if (table[2560].length == 0u) {
            return PDG_ERR_UNSUPPORTED;
        }
        rc = writer_put_bits(writer, table[2560].bits, table[2560].length);
        if (rc != PDG_OK) {
            return rc;
        }
        remaining -= 2560;
    }
    if (remaining <= 63) {
        if (table[remaining].length == 0u) {
            return PDG_ERR_UNSUPPORTED;
        }
        return writer_put_bits(writer, table[remaining].bits, table[remaining].length);
    }
    {
        int makeup = (remaining / 64) * 64;
        if (makeup > 2560) {
            makeup = 2560;
        }
        if (table[makeup].length == 0u) {
            return PDG_ERR_UNSUPPORTED;
        }
        rc = writer_put_bits(writer, table[makeup].bits, table[makeup].length);
        if (rc != PDG_OK) {
            return rc;
        }
        remaining -= makeup;
        if (remaining < 0 || remaining > 63 || table[remaining].length == 0u) {
            return PDG_ERR_UNSUPPORTED;
        }
        return writer_put_bits(writer, table[remaining].bits, table[remaining].length);
    }
}

static int bit_at(const uint8_t *bits, size_t stride, uint32_t x)
{
    return (bits[(size_t)(x >> 3)] >> (7u - (x & 7u))) & 1u;
}

static int encode_row(FaxBitWriter *writer, const uint8_t *row, uint32_t width)
{
    size_t stride = ((size_t)width + 7u) / 8u;
    uint32_t x = 0u;
    int color = 0; /* white */
    int rc;

    while (x < width) {
        uint32_t start = x;
        while (x < width && bit_at(row, stride, x) == (uint32_t)color) {
            ++x;
        }
        /* Runs alternate white/black; every pair is one horizontal mode. */
        if (color == 0) {
            uint32_t white_run = x - start;
            uint32_t black_start = x;
            while (x < width && bit_at(row, stride, x) == 1u) {
                ++x;
            }
            rc = writer_put_bits(writer, FAX_HORIZONTAL_CODE, FAX_HORIZONTAL_BITS);
            if (rc != PDG_OK) {
                return rc;
            }
            rc = writer_put_run(writer, (int)white_run, 0);
            if (rc != PDG_OK) {
                return rc;
            }
            rc = writer_put_run(writer, (int)(x - black_start), 1);
            if (rc != PDG_OK) {
                return rc;
            }
        } else {
            /* A line can only start in black through a zero-length white run,
             * which the white branch above already emitted. */
            return PDG_ERR_FORMAT;
        }
    }
    return PDG_OK;
}

int pdg_encode_g4_horizontal(const uint8_t *bits, size_t bits_size,
                             uint32_t width, uint32_t height,
                             uint8_t **out_data, size_t *out_size)
{
    FaxBitWriter writer;
    size_t stride;
    uint32_t y;
    int rc;

    if (!bits || !out_data || !out_size || !width || !height) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    stride = ((size_t)width + 7u) / 8u;
    if (bits_size < stride * (size_t)height) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    *out_data = NULL;
    *out_size = 0u;
    ensure_run_codes();
    memset(&writer, 0, sizeof(writer));
    for (y = 0u; y < height; ++y) {
        rc = encode_row(&writer, bits + (size_t)y * stride, width);
        if (rc != PDG_OK) {
            free(writer.data);
            return rc;
        }
    }
    rc = writer_put_bits(&writer, FAX_EOFB_VALUE, FAX_EOFB_BITS);
    if (rc != PDG_OK) {
        free(writer.data);
        return rc;
    }
    {
        size_t bytes = (writer.bit_count + 7u) / 8u;
        *out_data = writer.data;
        *out_size = bytes;
    }
    return PDG_OK;
}
