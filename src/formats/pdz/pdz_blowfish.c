#include "pdz_internal.h"
#include "../../core/byte_utils.h"

#include <string.h>

/*
 * Blowfish-ECB, as the official engine applies it (pdgDll!0x1000b780 calling
 * 0x10001290):
 *
 *     engine_decrypt(block) = word_swap32( Blowfish_ECB_decrypt( word_swap32(block) ) )
 *
 * The word swap is the part that is easy to miss, and missing it is why an
 * earlier round of key testing in this project produced false negatives.
 */

static uint32_t blowfish_f(const uint32_t s[4][256], uint32_t x)
{
    return ((s[0][(x >> 24) & 0xFFu] + s[1][(x >> 16) & 0xFFu]) ^ s[2][(x >> 8) & 0xFFu])
           + s[3][x & 0xFFu];
}

static void blowfish_encrypt_block(const uint32_t p[18],
                                   const uint32_t s[4][256],
                                   uint32_t *left,
                                   uint32_t *right)
{
    uint32_t l = *left;
    uint32_t r = *right;

    for (int i = 0; i < 16; ++i) {
        l ^= p[i];
        r ^= blowfish_f(s, l);
        uint32_t swap = l;
        l = r;
        r = swap;
    }
    uint32_t swap = l;
    l = r;
    r = swap;
    r ^= p[16];
    l ^= p[17];

    *left = l;
    *right = r;
}

static void blowfish_decrypt_block(const uint32_t p[18],
                                   const uint32_t s[4][256],
                                   uint32_t *left,
                                   uint32_t *right)
{
    uint32_t l = *left;
    uint32_t r = *right;

    for (int i = 17; i >= 2; --i) {
        l ^= p[i];
        r ^= blowfish_f(s, l);
        uint32_t swap = l;
        l = r;
        r = swap;
    }
    uint32_t swap = l;
    l = r;
    r = swap;
    r ^= p[1];
    l ^= p[0];

    *left = l;
    *right = r;
}

static void blowfish_expand_key(const uint8_t *key, size_t key_length, uint32_t p[18],
                                uint32_t s[4][256])
{
    memcpy(p, pdz_blowfish_p_init, sizeof(pdz_blowfish_p_init));
    memcpy(s, pdz_blowfish_s_init, sizeof(pdz_blowfish_s_init));

    size_t cursor = 0;
    for (int i = 0; i < 18; ++i) {
        uint32_t word = 0;
        for (int byte = 0; byte < 4; ++byte) {
            word = (word << 8) | key[cursor % key_length];
            ++cursor;
        }
        p[i] ^= word;
    }

    uint32_t l = 0;
    uint32_t r = 0;
    for (int i = 0; i < 18; i += 2) {
        blowfish_encrypt_block(p, s, &l, &r);
        p[i] = l;
        p[i + 1] = r;
    }
    for (int box = 0; box < 4; ++box) {
        for (int i = 0; i < 256; i += 2) {
            blowfish_encrypt_block(p, s, &l, &r);
            s[box][i] = l;
            s[box][i + 1] = r;
        }
    }
}

void pdz_word_swap32(uint8_t *data, size_t size)
{
    for (size_t i = 0; i + 4 <= size; i += 4) {
        uint8_t t0 = data[i + 0];
        uint8_t t1 = data[i + 1];
        data[i + 0] = data[i + 3];
        data[i + 1] = data[i + 2];
        data[i + 2] = t1;
        data[i + 3] = t0;
    }
}

void pdz_blowfish_ecb_decrypt(uint8_t *data, size_t size, const uint8_t key[16])
{
    const size_t usable = size - (size % 8);
    if (usable < 0x20) {
        return;
    }

    /* ~4 KiB of key schedule; the engine keeps the same tables per call. */
    uint32_t p[18];
    uint32_t s[4][256];
    blowfish_expand_key(key, 16, p, s);

    pdz_word_swap32(data, usable);
    for (size_t i = 0; i + 8 <= usable; i += 8) {
        uint32_t l = qw_read_u32_be(data + i);
        uint32_t r = qw_read_u32_be(data + i + 4);
        blowfish_decrypt_block(p, s, &l, &r);
        qw_write_u32_be(data + i, l);
        qw_write_u32_be(data + i + 4, r);
    }
    pdz_word_swap32(data, usable);
}
