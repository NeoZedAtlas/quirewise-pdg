#include "pdg_internal.h"

#include <string.h>

typedef struct Md5Context {
    uint32_t state[4];
    uint64_t bit_count;
    uint8_t buffer[64];
} Md5Context;

static uint32_t rotl32(uint32_t value, uint32_t bits)
{
    return (value << bits) | (value >> (32 - bits));
}

static void md5_transform(uint32_t state[4], const uint8_t block[64])
{
    static const uint32_t k[64] = {
        0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
        0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
        0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
        0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
        0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
        0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
        0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
        0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u, 0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
    };
    static const uint8_t s[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
    };
    uint32_t m[16];
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];

    for (int i = 0; i < 16; ++i) {
        m[i] = qw_read_u32_le(block + i * 4);
    }

    for (uint32_t i = 0; i < 64; ++i) {
        uint32_t f;
        uint32_t g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) & 15;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) & 15;
        }
        uint32_t next = d;
        d = c;
        c = b;
        b += rotl32(a + f + k[i] + m[g], s[i]);
        a = next;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

static void md5_init(Md5Context *ctx)
{
    ctx->state[0] = 0x67452301u;
    ctx->state[1] = 0xefcdab89u;
    ctx->state[2] = 0x98badcfeu;
    ctx->state[3] = 0x10325476u;
    ctx->bit_count = 0;
    memset(ctx->buffer, 0, sizeof(ctx->buffer));
}

static void md5_update(Md5Context *ctx, const uint8_t *data, size_t size)
{
    size_t used = (size_t)((ctx->bit_count >> 3) & 63);
    ctx->bit_count += (uint64_t)size * 8u;

    if (used) {
        size_t free_space = 64 - used;
        if (size < free_space) {
            memcpy(ctx->buffer + used, data, size);
            return;
        }
        memcpy(ctx->buffer + used, data, free_space);
        md5_transform(ctx->state, ctx->buffer);
        data += free_space;
        size -= free_space;
    }

    while (size >= 64) {
        md5_transform(ctx->state, data);
        data += 64;
        size -= 64;
    }
    if (size) {
        memcpy(ctx->buffer, data, size);
    }
}

static void md5_final(Md5Context *ctx, uint8_t digest[16])
{
    static const uint8_t padding[64] = {0x80};
    uint8_t length[8];
    uint64_t bit_count = ctx->bit_count;
    size_t used = (size_t)((bit_count >> 3) & 63);
    size_t pad_size = used < 56 ? 56 - used : 120 - used;

    for (int i = 0; i < 8; ++i) {
        length[i] = (uint8_t)(bit_count >> (8 * i));
    }
    md5_update(ctx, padding, pad_size);
    md5_update(ctx, length, sizeof(length));

    for (int i = 0; i < 4; ++i) {
        qw_write_u32_le(digest + i * 4, ctx->state[i]);
    }
}

void pdg_md5(const uint8_t *data, size_t size, uint8_t digest[16])
{
    Md5Context ctx;
    md5_init(&ctx);
    md5_update(&ctx, data, size);
    md5_final(&ctx, digest);
}
