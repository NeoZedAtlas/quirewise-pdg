/*
 * Self-contained MD5 and AES-256 for the SSCT certificate decoder.
 *
 * The C core has no third-party dependencies, so both primitives are provided
 * here.  AES is only ever used to DECRYPT (the certificate is read, never
 * written), and only in 16-byte ECB units -- CBC chaining is done by the
 * caller in pdz_ssct.c, matching pdgDll!FUN_10004bf0.
 *
 * Verified against the engine: pdzDll's SSCT cipher is a 14-round 128-bit
 * T-table cipher (pdfPdgDll!FUN_1000a570 is the same shape in the other DLL),
 * i.e. AES-256, with the certificate key schedule of FUN_10004650.
 */

#include "pdz_internal.h"

#include <string.h>

/* ------------------------------------------------------------------- MD5 */

typedef struct {
    uint32_t h[4];
    uint64_t total;
    uint8_t block[64];
    size_t used;
} PdzMd5;

static uint32_t md5_rotl(uint32_t v, int n)
{
    return (v << n) | (v >> (32 - n));
}

static const uint32_t md5_k[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au,
    0xa8304613u, 0xfd469501u, 0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u,
    0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u,
    0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u, 0x289b7ec6u, 0xeaa127fau,
    0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u,
    0xffeff47du, 0x85845dd1u, 0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u};

static const uint8_t md5_s[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

static void md5_block(PdzMd5 *c, const uint8_t *p)
{
    uint32_t m[16], a, b, cc, d, f, g, tmp;
    int i;

    for (i = 0; i < 16; i++) {
        m[i] = (uint32_t)p[4 * i] | ((uint32_t)p[4 * i + 1] << 8) |
               ((uint32_t)p[4 * i + 2] << 16) | ((uint32_t)p[4 * i + 3] << 24);
    }
    a = c->h[0];
    b = c->h[1];
    cc = c->h[2];
    d = c->h[3];
    for (i = 0; i < 64; i++) {
        if (i < 16) {
            f = (b & cc) | (~b & d);
            g = (uint32_t)i;
        } else if (i < 32) {
            f = (d & b) | (~d & cc);
            g = (uint32_t)(5 * i + 1) & 15u;
        } else if (i < 48) {
            f = b ^ cc ^ d;
            g = (uint32_t)(3 * i + 5) & 15u;
        } else {
            f = cc ^ (b | ~d);
            g = (uint32_t)(7 * i) & 15u;
        }
        /* F = F + A + K[i] + M[g];  A = D; D = C; C = B; B = B + rotl(F, s) */
        tmp = a;
        a = d;
        d = cc;
        cc = b;
        b = b + md5_rotl(tmp + f + md5_k[i] + m[g], md5_s[i]);
    }
    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += cc;
    c->h[3] += d;
}

void pdz_md5_init(PdzMd5 *c)
{
    c->h[0] = 0x67452301u;
    c->h[1] = 0xefcdab89u;
    c->h[2] = 0x98badcfeu;
    c->h[3] = 0x10325476u;
    c->total = 0;
    c->used = 0;
}

void pdz_md5_update(PdzMd5 *c, const void *data, size_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    c->total += size;
    while (size) {
        size_t take = 64 - c->used;
        if (take > size) {
            take = size;
        }
        memcpy(c->block + c->used, p, take);
        c->used += take;
        p += take;
        size -= take;
        if (c->used == 64) {
            md5_block(c, c->block);
            c->used = 0;
        }
    }
}

void pdz_md5_final(PdzMd5 *c, uint8_t out[16])
{
    uint64_t bits = c->total * 8u;
    uint8_t pad[72];
    size_t padlen = (c->used < 56) ? (56 - c->used) : (120 - c->used);
    int i;

    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    for (i = 0; i < 8; i++) {
        pad[padlen + i] = (uint8_t)(bits >> (8 * i));
    }
    pdz_md5_update(c, pad, padlen + 8);
    for (i = 0; i < 4; i++) {
        out[4 * i] = (uint8_t)(c->h[i]);
        out[4 * i + 1] = (uint8_t)(c->h[i] >> 8);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 3] = (uint8_t)(c->h[i] >> 24);
    }
}

void pdz_md5_digest(const void *data, size_t size, uint8_t out[16])
{
    PdzMd5 c;
    pdz_md5_init(&c);
    pdz_md5_update(&c, data, size);
    pdz_md5_final(&c, out);
}

void pdz_md5_hex_upper(const void *data, size_t size, char out[33])
{
    static const char digits[] = "0123456789ABCDEF";
    uint8_t d[16];
    int i;

    pdz_md5_digest(data, size, d);
    for (i = 0; i < 16; i++) {
        out[2 * i] = digits[d[i] >> 4];
        out[2 * i + 1] = digits[d[i] & 15];
    }
    out[32] = 0;
}

/* ------------------------------------------------------------------- AES */

static const uint8_t aes_sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b,
    0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0,
    0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26,
    0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2,
    0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0,
    0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed,
    0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f,
    0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
    0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec,
    0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14,
    0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c,
    0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d,
    0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f,
    0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e,
    0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
    0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f,
    0xb0, 0x54, 0xbb, 0x16};

static uint8_t aes_inv_sbox[256];
static int aes_inv_ready = 0;

static void aes_build_inv(void)
{
    int i;
    if (aes_inv_ready) {
        return;
    }
    for (i = 0; i < 256; i++) {
        aes_inv_sbox[aes_sbox[i]] = (uint8_t)i;
    }
    aes_inv_ready = 1;
}

static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t p = 0;
    int i;
    for (i = 0; i < 8; i++) {
        if (b & 1) {
            p ^= a;
        }
        b >>= 1;
        if (a & 0x80) {
            a = (uint8_t)((a << 1) ^ 0x1B);
        } else {
            a = (uint8_t)(a << 1);
        }
    }
    return p;
}

/* AES-256: Nk = 8 words, Nr = 14 rounds -> 60 words of key schedule. */
static void aes256_expand(const uint8_t key[32], uint8_t rk[240])
{
    static const uint8_t rcon[8] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};
    uint32_t w[60];
    int i, j;

    for (i = 0; i < 8; i++) {
        w[i] = ((uint32_t)key[4 * i] << 24) | ((uint32_t)key[4 * i + 1] << 16) |
               ((uint32_t)key[4 * i + 2] << 8) | (uint32_t)key[4 * i + 3];
    }
    for (i = 8; i < 60; i++) {
        uint32_t t = w[i - 1];
        if (i % 8 == 0) {
            t = (t << 8) | (t >> 24); /* RotWord */
            t = ((uint32_t)aes_sbox[(t >> 24) & 0xFF] << 24) |
                ((uint32_t)aes_sbox[(t >> 16) & 0xFF] << 16) |
                ((uint32_t)aes_sbox[(t >> 8) & 0xFF] << 8) |
                (uint32_t)aes_sbox[t & 0xFF];
            t ^= (uint32_t)rcon[i / 8 - 1] << 24;
        } else if (i % 8 == 4) {
            t = ((uint32_t)aes_sbox[(t >> 24) & 0xFF] << 24) |
                ((uint32_t)aes_sbox[(t >> 16) & 0xFF] << 16) |
                ((uint32_t)aes_sbox[(t >> 8) & 0xFF] << 8) |
                (uint32_t)aes_sbox[t & 0xFF];
        }
        w[i] = w[i - 8] ^ t;
    }
    for (i = 0; i < 60; i++) {
        for (j = 0; j < 4; j++) {
            rk[4 * i + j] = (uint8_t)(w[i] >> (24 - 8 * j));
        }
    }
}

static void add_round_key(uint8_t s[16], const uint8_t *rk)
{
    int i;
    for (i = 0; i < 16; i++) {
        s[i] ^= rk[i];
    }
}

static void inv_shift_rows(uint8_t s[16])
{
    uint8_t t;
    t = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = t;
    t = s[2]; s[2] = s[10]; s[10] = t;
    t = s[6]; s[6] = s[14]; s[14] = t;
    t = s[3]; s[3] = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = t;
}

static void inv_sub_bytes(uint8_t s[16])
{
    int i;
    for (i = 0; i < 16; i++) {
        s[i] = aes_inv_sbox[s[i]];
    }
}

static void inv_mix_columns(uint8_t s[16])
{
    int c;
    for (c = 0; c < 4; c++) {
        uint8_t *p = s + 4 * c;
        uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
        p[0] = (uint8_t)(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
        p[1] = (uint8_t)(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
        p[2] = (uint8_t)(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
        p[3] = (uint8_t)(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
    }
}

void pdz_aes256_decrypt_block(const uint8_t in[16], const uint8_t key[32], uint8_t out[16])
{
    uint8_t rk[240];
    uint8_t s[16];
    int round;

    aes_build_inv();
    aes256_expand(key, rk);
    memcpy(s, in, 16);

    add_round_key(s, rk + 14 * 16);
    for (round = 13; round >= 1; round--) {
        inv_shift_rows(s);
        inv_sub_bytes(s);
        add_round_key(s, rk + round * 16);
        inv_mix_columns(s);
    }
    inv_shift_rows(s);
    inv_sub_bytes(s);
    add_round_key(s, rk);
    memcpy(out, s, 16);
}
