#include "pdg_internal.h"

#include <string.h>

static uint32_t ror32(uint32_t value, unsigned bits)
{
    return (value >> bits) | (value << (32u - bits));
}

static uint32_t deslike_round(uint32_t value, const uint32_t schedule[32], int round)
{
    uint32_t a = (value ^ schedule[round * 2]) & 0x3f3f3f3fu;
    uint32_t b = (schedule[round * 2 + 1] ^ ror32(value, 4)) & 0x3f3f3f3fu;

    return pdg_des_round_sp[(a >> 0) & 0xff] |
           pdg_des_round_sp[((b >> 0) & 0xff) + 64] |
           pdg_des_round_sp[((a >> 8) & 0xff) + 128] |
           pdg_des_round_sp[((b >> 8) & 0xff) + 192] |
           pdg_des_round_sp[((a >> 16) & 0xff) + 256] |
           pdg_des_round_sp[((b >> 16) & 0xff) + 320] |
           pdg_des_round_sp[((a >> 24) & 0xff) + 384] |
           pdg_des_round_sp[((b >> 24) & 0xff) + 448];
}

static void deslike_schedule(const uint8_t key[8], uint32_t schedule[32])
{
    uint32_t left = qw_read_u32_le(key);
    uint32_t right = qw_read_u32_le(key + 4);

    uint32_t mix = (left ^ (right >> 4)) & 0x0f0f0f0fu;
    uint32_t c = mix ^ left;
    uint32_t d = (mix << 4) ^ right;

    mix = (d ^ (c >> 4)) & 0x01010101u;
    c = (mix << 4) ^ c;
    d = mix ^ d;

    mix = (c ^ (c >> 9)) & 0x00550055u;
    c = mix ^ (mix << 9) ^ c;
    mix = (d ^ (d >> 9)) & 0x00550055u;
    d = mix ^ (mix << 9) ^ d;

    mix = ((c & 0xffffu) ^ (c >> 18)) & 0x3333u;
    c = mix ^ (mix << 18) ^ c;
    mix = ((d & 0xffffu) ^ (d >> 18)) & 0x3333u;
    d = mix ^ (mix << 18) ^ d;

    uint32_t low = (uint8_t)(d ^ (d >> 16));
    d = low ^ (low << 16) ^ d;

    for (int round = 0; round < 16; ++round) {
        uint32_t c28 = c & 0x0fffffffu;
        uint32_t d28 = d & 0x0fffffffu;
        if (pdg_des_key_rot[round]) {
            c = (c28 >> 2) | (c << 26);
            d = (d28 >> 2) | (d28 << 26);
        } else {
            c = (c28 >> 1) | (c << 27);
            d = (d28 >> 1) | (d << 27);
        }

        uint8_t c0 = (uint8_t)c;
        uint8_t c1 = (uint8_t)(c >> 8);
        uint8_t c2 = (uint8_t)(c >> 16);
        uint8_t c3 = (uint8_t)(c >> 24);
        uint8_t d0 = (uint8_t)d;
        uint8_t d1 = (uint8_t)(d >> 8);
        uint8_t d3 = (uint8_t)(d >> 24);

        uint32_t c_index2 = (uint32_t)((c2 & 0x1d) | (((c2 & 0x40) | ((c2 >> 5) & 4)) >> 1));
        uint32_t c_index3 = (uint32_t)((c3 & 0x0e) | (((c1 >> 5) | (c0 & 0xc0)) >> 2));
        uint32_t left_half =
            pdg_des_key_pc[c & 0x3f] |
            pdg_des_key_pc[((c1 >> 1) & 0x3f) + 64] |
            pdg_des_key_pc[c_index2 + 128] |
            pdg_des_key_pc[c_index3 + 192];

        uint32_t right_half =
            pdg_des_key_pc[((d1 & 0x3d) | ((d1 >> 6) & 2)) + 320] |
            pdg_des_key_pc[((d3 & 0x0d) | ((((d >> 16) & 0xc0) | ((d0 >> 4) & 8)) >> 2)) + 448] |
            pdg_des_key_pc[((d >> 16) & 0x3f) + 384] |
            pdg_des_key_pc[(d & 0x3f) + 256];

        schedule[round * 2] = (left_half & 0xffffu) | (right_half << 16);
        schedule[round * 2 + 1] = (left_half >> 16) | (right_half & 0xffff0000u);
    }
}

static void deslike_block(const uint8_t in[8], uint8_t out[8], const uint32_t schedule[32])
{
    uint32_t left = qw_read_u32_le(in);
    uint32_t right = qw_read_u32_le(in + 4);

    uint32_t mix = (left ^ (right >> 4)) & 0x0f0f0f0fu;
    uint32_t a = mix ^ left;
    uint32_t b = (mix << 4) ^ right;
    mix = (b ^ (a >> 16)) & 0xffffu;
    b = mix ^ b;
    a = (mix << 16) ^ a;
    mix = (a ^ (b >> 2)) & 0x33333333u;
    a = mix ^ a;
    b = (mix << 2) ^ b;
    mix = (b ^ (a >> 8)) & 0x00ff00ffu;
    b = mix ^ b;
    a = (mix << 8) ^ a;
    mix = (a ^ (b >> 1)) & 0x55555555u;

    uint32_t x = mix ^ a;
    uint32_t y = (mix << 1) ^ b;
    for (int round = 0; round < 16; ++round) {
        uint32_t next = deslike_round(x, schedule, round) ^ y;
        y = x;
        x = next;
    }

    mix = (x ^ (y >> 1)) & 0x55555555u;
    a = mix ^ x;
    b = (mix << 1) ^ y;
    mix = (b ^ (a >> 8)) & 0x00ff00ffu;
    b = mix ^ b;
    a = (mix << 8) ^ a;
    mix = (a ^ (b >> 2)) & 0x33333333u;
    a = mix ^ a;
    b = (mix << 2) ^ b;
    mix = (b ^ (a >> 16)) & 0xffffu;
    b = mix ^ b;
    a = (mix << 16) ^ a;
    mix = (a ^ (b >> 4)) & 0x0f0f0f0fu;

    qw_write_u32_le(out, b ^ (mix << 4));
    qw_write_u32_le(out + 4, a ^ mix);
}

static void xor8(uint8_t out[8], const uint8_t a[8], const uint8_t b[8])
{
    for (int i = 0; i < 8; ++i) {
        out[i] = (uint8_t)(a[i] ^ b[i]);
    }
}

void pdg_deslike_derive_schedules(const uint8_t key16[16], uint32_t schedules[6][32])
{
    static const uint8_t default_key[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
    uint32_t default_schedule[32];
    uint8_t block[8];
    uint8_t temp[8];

    deslike_schedule(default_key, default_schedule);
    deslike_block(key16, block, default_schedule);
    deslike_schedule(block, schedules[0]);

    xor8(temp, block, key16 + 8);
    deslike_block(temp, block, default_schedule);
    deslike_schedule(block, schedules[1]);

    xor8(temp, block, key16);
    temp[0] ^= 0x80;
    deslike_block(temp, block, default_schedule);
    deslike_schedule(block, schedules[2]);

    xor8(temp, block, key16 + 8);
    temp[0] ^= 0x40;
    deslike_block(temp, block, default_schedule);
    deslike_schedule(block, schedules[3]);

    xor8(temp, block, key16);
    temp[0] ^= 0x20;
    deslike_block(temp, block, default_schedule);
    deslike_schedule(block, schedules[4]);

    xor8(temp, block, key16 + 8);
    temp[0] ^= 0x10;
    deslike_block(temp, block, default_schedule);
    deslike_schedule(block, schedules[5]);
}

void pdg_deslike_transform16(uint8_t block[16], const uint32_t schedules[6][32])
{
    uint8_t round_out[8];

    deslike_block(block + 8, round_out, schedules[5]);
    for (int i = 0; i < 8; ++i) block[i] ^= round_out[i];

    deslike_block(block, round_out, schedules[4]);
    for (int i = 0; i < 8; ++i) block[8 + i] ^= round_out[i];

    deslike_block(block + 8, round_out, schedules[3]);
    for (int i = 0; i < 8; ++i) block[i] ^= round_out[i];

    deslike_block(block, round_out, schedules[2]);
    for (int i = 0; i < 8; ++i) block[8 + i] ^= round_out[i];

    deslike_block(block + 8, round_out, schedules[1]);
    for (int i = 0; i < 8; ++i) block[i] ^= round_out[i];

    deslike_block(block, round_out, schedules[0]);
    for (int i = 0; i < 8; ++i) block[8 + i] ^= round_out[i];
}
