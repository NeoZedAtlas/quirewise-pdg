/* CCITT Group 4 encoder coverage.
 *
 * The package decoder (qw_pdg_decode_g4) implements SSReader's private Group 4
 * dialect, so it cannot judge a standard T.6 stream. The goldens below were
 * produced by this encoder and verified pixel for pixel with MuPDF (mutool
 * draw against a /CCITTFaxDecode page with /BlackIs1 true) before being
 * recorded here, so they lock in a stream a conforming decoder accepts. */

#include <pdg.h>

#include "../src/formats/pdg/pdg_fax.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

#define GOLDEN_WIDTH 16u
#define GOLDEN_HEIGHT 4u

typedef struct Golden {
    int pattern;
    const uint8_t *bytes;
    size_t size;
} Golden;

static const uint8_t kPattern0[] = {0x35u, 0x06u, 0xe6u, 0xa0u, 0xdcu, 0xd4u, 0x1bu,
                                    0x9au, 0x83u, 0x70u, 0x01u, 0x00u, 0x10u};
static const uint8_t kPattern1[] = {0x26u, 0xa0u, 0xb9u, 0x35u, 0x05u, 0xc9u, 0xa8u,
                                    0x2eu, 0x4du, 0x41u, 0x70u, 0x01u, 0x00u, 0x10u};
static const uint8_t kPattern2[] = {0x31u, 0x18u, 0x8cu, 0x23u, 0x11u, 0x88u, 0xc2u,
                                    0x31u, 0x18u, 0x8cu, 0x23u, 0x11u, 0x88u, 0xc2u,
                                    0x00u, 0x10u, 0x01u};
static const uint8_t kPattern3[] = {0x26u, 0xa0u, 0xb9u, 0x70u, 0x73u, 0x00u,
                                    0x86u, 0xc3u, 0x80u, 0x08u, 0x00u, 0x80u};
static const uint8_t kPattern4[] = {0x37u, 0x18u, 0x65u, 0xc3u, 0x73u, 0x11u, 0xb8u,
                                    0xc0u, 0x6eu, 0x62u, 0x31u, 0x1bu, 0x0du, 0xcbu,
                                    0xc6u, 0x19u, 0x84u, 0x00u, 0x20u, 0x02u};
static const uint8_t kPattern5[] = {0x26u, 0xb1u, 0x3cu, 0x7au, 0x3eu, 0x1bu, 0x93u,
                                    0x5cu, 0x9eu, 0x23u, 0x86u, 0xe7u, 0x23u, 0x30u,
                                    0xdcu, 0x00u, 0x40u, 0x04u};

static const Golden kGoldens[] = {
    {0, kPattern0, sizeof(kPattern0)},
    {1, kPattern1, sizeof(kPattern1)},
    {2, kPattern2, sizeof(kPattern2)},
    {3, kPattern3, sizeof(kPattern3)},
    {4, kPattern4, sizeof(kPattern4)},
    {5, kPattern5, sizeof(kPattern5)},
};

static void set_bit(uint8_t *bits, size_t stride, uint32_t x, uint32_t y, int value)
{
    uint8_t *row = bits + (size_t)y * stride;
    uint8_t mask = (uint8_t)(0x80u >> (x & 7u));
    if (value) {
        row[x >> 3] |= mask;
    } else {
        row[x >> 3] &= (uint8_t)~mask;
    }
}

static void fill_pattern(uint8_t *bits, size_t stride, uint32_t width, uint32_t height,
                         int pattern)
{
    uint32_t x;
    uint32_t y;
    for (y = 0u; y < height; ++y) {
        for (x = 0u; x < width; ++x) {
            int value = 0;
            switch (pattern) {
            case 0: value = 0; break;
            case 1: value = 1; break;
            case 2: value = (int)((x / 3u) & 1u); break;
            case 3: value = ((y % 5u) == 0u) ? 1 : ((x > (y % 97u)) ? 1 : 0); break;
            case 4: value = (int)(((x * 1103515245u) + (y * 12345u) + (x ^ y)) >> 16) & 1; break;
            default: value = (int)(((x + y * 7u) % 13u) < 3u); break;
            }
            set_bit(bits, stride, x, y, value);
        }
    }
}

static void test_goldens(void)
{
    size_t stride = ((size_t)GOLDEN_WIDTH + 7u) / 8u;
    size_t index;
    for (index = 0u; index < sizeof(kGoldens) / sizeof(kGoldens[0]); ++index) {
        uint8_t bits[2u * GOLDEN_HEIGHT];
        uint8_t *encoded = NULL;
        size_t encoded_size = 0u;
        int rc;
        memset(bits, 0, sizeof(bits));
        fill_pattern(bits, stride, GOLDEN_WIDTH, GOLDEN_HEIGHT, kGoldens[index].pattern);
        rc = pdg_encode_g4_horizontal(bits, sizeof(bits), GOLDEN_WIDTH,
                                      GOLDEN_HEIGHT, &encoded, &encoded_size);
        CHECK(rc == PDG_OK);
        if (rc == PDG_OK) {
            CHECK(encoded != NULL);
            if (encoded_size != kGoldens[index].size) {
                fprintf(stderr, "pattern %d: got %zu bytes, golden %zu\n",
                        kGoldens[index].pattern, encoded_size, kGoldens[index].size);
            }
            CHECK(encoded_size == kGoldens[index].size);
            if (encoded_size == kGoldens[index].size) {
                CHECK(memcmp(encoded, kGoldens[index].bytes, encoded_size) == 0);
            }
        }
        free(encoded);
    }
}

static void test_encoding_is_deterministic(void)
{
    uint8_t bits[2u * GOLDEN_HEIGHT];
    uint8_t *first = NULL;
    uint8_t *second = NULL;
    size_t first_size = 0u;
    size_t second_size = 0u;
    memset(bits, 0, sizeof(bits));
    fill_pattern(bits, 2u, GOLDEN_WIDTH, GOLDEN_HEIGHT, 5);
    CHECK(pdg_encode_g4_horizontal(bits, sizeof(bits), GOLDEN_WIDTH, GOLDEN_HEIGHT,
                                   &first, &first_size) == PDG_OK);
    CHECK(pdg_encode_g4_horizontal(bits, sizeof(bits), GOLDEN_WIDTH, GOLDEN_HEIGHT,
                                   &second, &second_size) == PDG_OK);
    CHECK(first_size == second_size);
    CHECK(first_size == 0u || memcmp(first, second, first_size) == 0);
    free(first);
    free(second);
}

static void test_invalid_arguments(void)
{
    uint8_t bits[8];
    uint8_t *out = NULL;
    size_t out_size = 0u;
    memset(bits, 0, sizeof(bits));
    CHECK(pdg_encode_g4_horizontal(NULL, sizeof(bits), 8u, 8u, &out, &out_size) ==
          PDG_ERR_INVALID_ARGUMENT);
    CHECK(pdg_encode_g4_horizontal(bits, sizeof(bits), 0u, 8u, &out, &out_size) ==
          PDG_ERR_INVALID_ARGUMENT);
    CHECK(pdg_encode_g4_horizontal(bits, sizeof(bits), 8u, 0u, &out, &out_size) ==
          PDG_ERR_INVALID_ARGUMENT);
    /* The buffer must hold width * height bits. */
    CHECK(pdg_encode_g4_horizontal(bits, sizeof(bits), 64u, 64u, &out, &out_size) ==
          PDG_ERR_INVALID_ARGUMENT);
}

static void test_render_to_ccitt_refuses_native_pages(void)
{
    static const unsigned char jpeg_prefix[] = {0xffu, 0xd8u, 0xffu, 0xe0u,
                                                0x00u, 0x10u, 'J', 'F', 'I', 'F'};
    PdgBuffer out;
    int rc;

    memset(&out, 0, sizeof(out));
    rc = pdg_render_to_ccitt(jpeg_prefix, sizeof(jpeg_prefix), &out);
    CHECK(rc != PDG_OK);
    CHECK(out.data == NULL);
    pdg_buffer_free(&out);

    memset(&out, 0, sizeof(out));
    rc = pdg_render_to_ccitt(NULL, 0u, &out);
    CHECK(rc == PDG_ERR_INVALID_ARGUMENT);
    pdg_buffer_free(&out);

    memset(&out, 0, sizeof(out));
    rc = pdg_render_to_ccitt("not a pdg", 9u, &out);
    CHECK(rc != PDG_OK);
    pdg_buffer_free(&out);
}

int main(void)
{
    test_goldens();
    test_encoding_is_deterministic();
    test_invalid_arguments();
    test_render_to_ccitt_refuses_native_pages();
    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    puts("PASS: CCITT Group 4 encoder goldens");
    return 0;
}


