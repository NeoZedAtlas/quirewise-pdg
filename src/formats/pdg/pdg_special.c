#include "pdg_internal.h"

#include <stdlib.h>
#include <string.h>

static const uint8_t SCRAMBLE_A[] =
    "132jksdiau34ukjasdflkj!@#234afdji78#%234ADf v\x1c=-\x07"
    "sdkj908342<ADF>/#w%4t5o8uwrouyouhj8934038p350|+_SDFadwr"
    "!234gasM,.spfdlp[WEey3dfsfgsf4890^:9!@#56&89384978907890"
    "54ioerua^3~!2&*(_+*(80w&uidfs9ghidfower239-89taerC(!7a";

static const uint8_t SCRAMBLE_B[] =
    "Afgasdkol!@#$564746Lfat0[9i FDLAas090132!$@##$2354(*^78~*54"
    "sdfakl;ml;lak;j2a#$2342!@#128-reg;o[werqPWEQRioADSF1234&*"
    "(6908_)55454!";

static int c_mod(int value, int divisor)
{
    return value % divisor;
}

static uint8_t u8(int value)
{
    return (uint8_t)value;
}

static int16_t i16(int value)
{
    return (int16_t)(uint16_t)value;
}

static void build_fibonacci_table(uint32_t out[32])
{
    out[0] = 1;
    out[1] = 1;
    for (int i = 2; i < 32; ++i) {
        out[i] = out[i - 1] + out[i - 2];
    }
}

static void build_index_table(uint32_t out[64])
{
    out[0] = 2;
    out[1] = 3;
    int state = 1;
    int value = state + 4;
    int count = 2;
    while (count < 64) {
        if (value != 0x19 && value != 0x31 && value != 0x79 && value != 0xa9 && value != 0x121) {
            out[count++] = (uint32_t)value;
        }
        ++state;
        if (state == 3) {
            value += 4;
            state = 1;
        } else {
            value += 2;
        }
    }
}

static void compute_scramble_positions(const uint8_t *header, uint32_t positions[24])
{
    uint32_t fib[32];
    uint32_t index[64];
    build_fibonacci_table(fib);
    build_index_table(index);

    uint8_t h5e = header[0x5e];
    uint16_t data_size16 = (uint16_t)(header[0x1c] | ((uint16_t)header[0x1d] << 8));
    uint8_t width_low = header[0x10];
    uint8_t height_low = header[0x12];
    uint8_t size_high = (uint8_t)(data_size16 >> 8);
    uint8_t sum_mix = u8(height_low + header[0x1c] + width_low);
    uint8_t diff_mix = u8(width_low - height_low);
    uint8_t or_mix = u8(width_low | height_low);
    uint8_t mul_mix = u8(width_low * height_low);
    int16_t run_mix = i16(sum_mix * diff_mix);
    uint8_t block_mix = u8(width_low * size_high);
    uint8_t seed = u8(h5e + width_low * height_low * width_low);

    uint32_t p0 = (or_mix * mul_mix + size_high * block_mix - run_mix + seed + 250) & 0x1ffu;
    uint32_t p12 = (or_mix * or_mix - block_mix * seed + run_mix * mul_mix + 250) & 0x1ffu;

    int mixed = block_mix * block_mix + sum_mix + diff_mix + or_mix + mul_mix + size_high + width_low;
    uint32_t v14 = SCRAMBLE_A[c_mod(mixed + SCRAMBLE_A[c_mod(seed, 0xd7)], 0xd7)] *
                   SCRAMBLE_B[c_mod(mixed + SCRAMBLE_B[c_mod(seed, 0x82)], 0x82)];
    uint32_t v15 = SCRAMBLE_A[c_mod(diff_mix * mul_mix - size_high, 0xd7)];
    uint32_t p1 = v14 & 0x1ffu;
    uint32_t v16 = fib[(data_size16 >> 8) & 0x1f];
    uint32_t p13 = (SCRAMBLE_B[c_mod(width_low, 0x82)] * v15) & 0x1ffu;
    uint32_t v17 = index[(width_low * size_high) & 0x3f];
    uint32_t p2 = v16 & 0x1ffu;
    uint32_t p14 = v17 & 0x1ffu;
    uint32_t p3 = index[((v17 & 0xffu) * SCRAMBLE_B[c_mod(p2 * SCRAMBLE_A[c_mod(p2, 0xd7)], 0x82)]) & 0x3f] & 0x1ffu;
    uint32_t p15 = fib[((v16 & 0xffu) * SCRAMBLE_A[c_mod(p14 * SCRAMBLE_B[c_mod(p14, 0x82)], 0xd7)]) & 0x1f] & 0x1ffu;
    int16_t short_mix = i16(p1 * p15);

    positions[0] = p0;
    positions[1] = p1;
    positions[2] = p2;
    positions[3] = p3;
    positions[4] = p0;
    positions[5] = (p12 * p1 - p0) & 0x1ffu;
    positions[6] = (p14 - p0 * p1) & 0x1ffu;
    positions[7] = (p3 - p12 * p14) & 0x1ffu;
    positions[8] = (p12 + p0) & 0x1ffu;
    positions[9] = (positions[5] + p13) & 0x1ffu;
    positions[10] = (positions[6] + (p12 * p13 + p2 * p14 - short_mix + p3 + 250) - p1) & 0x1ffu;
    positions[11] = (positions[7] + positions[8] - p15) & 0x1ffu;
    positions[12] = p12;
    positions[13] = p13;
    positions[14] = p14;
    positions[15] = p15;
    positions[16] = (p12 * short_mix - p14 * p3 + p13 * p13 + 250) & 0x1ffu;
    positions[17] = (p0 * p0 + p2 * p2 - p12 * p12) & 0x1ffu;
    positions[18] = (p12 * p13 + p2 * p14 - short_mix + p3 + 250) & 0x1ffu;
    positions[19] = (p3 * p15) & 0x1ffu;
    positions[20] = (SCRAMBLE_B[c_mod(p12, 0x82)] + SCRAMBLE_A[c_mod(positions[16], 0xd7)]) & 0x1ffu;
    positions[21] = index[((v14 & 0xffu) * u8((p12 & 0xffu) * (v14 & 0xffu) - (p0 & 0xffu) + (p13 & 0xffu))) & 0x3f] & 0x1ffu;
    positions[22] = index[SCRAMBLE_A[c_mod(positions[6], 0xd7)] & 0x3f] & 0x1ffu;
    positions[23] = fib[SCRAMBLE_B[c_mod(positions[6], 0x82)] & 0x1f] & 0x1ffu;
}

static void swap_xor_pair(uint8_t *data, uint16_t a, uint16_t b)
{
    uint16_t high = a, low = b;
    if (a < b) {
        high = b;
        low = a;
    }
    uint16_t distance = (uint16_t)((high - low == 0x11) ? 0x13 : 0x11);
    uint8_t high_value = (uint8_t)(data[high] ^ data[high + distance]);
    data[high] = (uint8_t)(data[low] ^ data[low + distance]);
    data[low] = high_value;
}

static int reverse_xor_span(uint8_t *data, uint16_t a, uint16_t b)
{
    uint16_t low = a, high = b;
    if (a > b) {
        low = b;
        high = a;
    }
    uint16_t count = (uint16_t)(high - low - 1);
    if (count == 0) {
        return PDG_OK;
    }
    uint8_t *temp = (uint8_t *)malloc(count);
    if (!temp) {
        return PDG_ERR_OUT_OF_MEMORY;
    }
    uint8_t key = (uint8_t)(data[low] ^ data[high]);
    for (uint16_t i = 0; i < count; ++i) {
        temp[count - i - 1] = (uint8_t)(key ^ data[low + 1 + i]);
    }
    memcpy(data + low + 1, temp, count);
    free(temp);
    return PDG_OK;
}

static void midpoint_sub(uint8_t *data, uint16_t a, uint16_t b)
{
    if (a == b) {
        return;
    }
    uint8_t value = data[a];
    uint16_t midpoint = (uint16_t)((a >> 1) + (b >> 1));
    if (midpoint != a) {
        value ^= data[midpoint];
    }
    data[a] = (uint8_t)(value - b);
}

int pdg_apply_lightweight_scramble(const uint8_t *header, size_t header_size, uint8_t *payload, size_t payload_size, uint8_t type)
{
    if (!header || !payload) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    if (type < 0x11 || type > 0x1c) {
        return PDG_OK;
    }
    if (header_size < PDG_HEADER_SIZE) {
        return PDG_ERR_FORMAT;
    }

    uint32_t positions[24];
    compute_scramble_positions(header, positions);
    uint16_t left = (uint16_t)positions[type - 0x11];
    uint16_t right = (uint16_t)positions[type + 12 - 0x11];
    if ((size_t)(left > right ? left : right) + 0x13 >= payload_size) {
        return PDG_ERR_FORMAT;
    }

    if (type <= 0x14) {
        swap_xor_pair(payload, left, right);
        return PDG_OK;
    }
    if (type <= 0x18) {
        return reverse_xor_span(payload, left, right);
    }
    midpoint_sub(payload, left, right);
    return PDG_OK;
}

static int transform_first_17_chunks(uint8_t *payload, size_t payload_size, const uint8_t key16[16])
{
    if (payload_size < 16 + 16 * 0x20) {
        return PDG_ERR_FORMAT;
    }
    uint32_t schedules[6][32];
    pdg_deslike_derive_schedules(key16, schedules);
    for (int i = 0; i < 17; ++i) {
        pdg_deslike_transform16(payload + i * 0x20, schedules);
    }
    return PDG_OK;
}

int pdg_apply_optional_layer(uint8_t *header, size_t header_size, uint8_t *payload, size_t payload_size)
{
    if (!header || !payload) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    if (header_size < PDG_HEADER_SIZE || header[0x38] == 0) {
        return PDG_OK;
    }
    if (header[0x39] != 0x86) {
        return PDG_ERR_UNSUPPORTED;
    }
    uint8_t key[16];
    qw_write_u32_le(key + 0, 0x32b43231u);
    qw_write_u32_le(key + 4, 0x381a3439u);
    qw_write_u32_le(key + 8, 0x00806504u);
    qw_write_u32_le(key + 12, 0x00920204u);

    int rc = transform_first_17_chunks(payload, payload_size, key);
    if (rc != PDG_OK) {
        return rc;
    }
    header[0x38] = 0;
    qw_write_u16_le(header + 0x10, (uint16_t)(qw_read_u16_le(header + 0x10) - 49));
    return PDG_OK;
}

int pdg_apply_a_family_transform(uint8_t *header, size_t header_size, uint8_t *payload, size_t payload_size, uint8_t type)
{
    if (!header || !payload) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    if (type != 0xaa && type != 0xab && type != 0xac) {
        return PDG_OK;
    }
    if (header_size < PDG_HEADER_SIZE) {
        return PDG_ERR_FORMAT;
    }

    uint8_t key[16];
    if (type == 0xaa) {
        qw_write_u32_le(key + 0, 0x00820208u);
        qw_write_u32_le(key + 4, 0x01220208u);
        qw_write_u32_le(key + 8, 0x00805038u);
        qw_write_u32_le(key + 12, 0x00920208u);
    } else if (type == 0xab) {
        uint16_t width = qw_read_u16_le(header + 0x10);
        qw_write_u16_le(header + 0x10, qw_read_u16_le(header + 0x12));
        qw_write_u16_le(header + 0x12, width);
        qw_write_u32_le(key + 0, qw_read_u32_le(header + 0x0c) ^ 0x00820208u);
        qw_write_u32_le(key + 4, qw_read_u32_le(header + 0x10) ^ 0x01220208u);
        qw_write_u32_le(key + 8, qw_read_u32_le(header + 0x14) ^ 0x00805038u);
        qw_write_u32_le(key + 12, qw_read_u32_le(header + 0x18) ^ 0x00920208u);
    } else {
        qw_write_u32_le(key + 0, qw_read_u32_le(header + 0x20) ^ 0x00820208u);
        qw_write_u32_le(key + 4, qw_read_u32_le(header + 0x24) ^ 0x01220208u);
        qw_write_u32_le(key + 8, qw_read_u32_le(header + 0x28) ^ 0x00805038u);
        qw_write_u32_le(key + 12, qw_read_u32_le(header + 0x2c) ^ 0x00920208u);
    }
    return transform_first_17_chunks(payload, payload_size, key);
}

int pdg_apply_type28_transform(const uint8_t *header, size_t header_size, uint8_t *payload, size_t payload_size)
{
    if (!header || !payload) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    if (header_size < PDG_HEADER_SIZE) {
        return PDG_ERR_FORMAT;
    }

    uint32_t d0 = qw_read_u32_le(header + 0x10);
    uint32_t d1 = qw_read_u32_le(header + 0x14);
    uint32_t d2 = qw_read_u32_le(header + 0x18);
    uint32_t d3 = qw_read_u32_le(header + 0x1c);
    uint32_t d4 = 0;
    uint32_t d5 = 0;
    uint32_t d6 = 0;
    uint32_t d7 = 0;
    uint32_t sum = 0xe3779b90u;

    uint32_t v0 = d2 - ((((d0 << 4) + d6) ^ ((d0 >> 5) + d5) ^ (d0 + sum)));
    uint32_t v1 = d3 - ((((v0 << 4) + d4) ^ ((v0 >> 5) + d7) ^ (v0 + sum)));
    uint32_t v2 = d1 - ((((v1 << 4) + d6) ^ ((v1 >> 5) + d7) ^ (v1 + sum)));
    uint32_t v3 = d0 - ((((v2 << 4) + d4) ^ ((v2 >> 5) + d5) ^ (v2 + sum)));

    uint32_t v4 = d4 - ((((d4 << 4) + v0) ^ ((d6 >> 5) + v0) ^ (v3 + sum)));
    uint32_t v5 = d5 - ((((d5 << 4) + d5) ^ ((d7 >> 5) + v1) ^ (v3 + sum)));
    uint32_t v6 = d6 - ((((d6 << 4) + d6) ^ ((v4 >> 5) + v3) ^ (v2 + sum)));
    uint32_t v7 = d7 - ((((d7 << 4) + d7) ^ ((d6 >> 5) + v2) ^ (v1 + sum)));

    uint8_t key[16];
    qw_write_u32_le(key + 0, v4);
    qw_write_u32_le(key + 4, v5);
    qw_write_u32_le(key + 8, v6);
    qw_write_u32_le(key + 12, v7);

    uint32_t schedules[6][32];
    pdg_deslike_derive_schedules(key, schedules);
    size_t chunk_count = payload_size / 0x20u;
    for (size_t i = 0; i < chunk_count; ++i) {
        pdg_deslike_transform16(payload + i * 0x20u, schedules);
    }
    return PDG_OK;
}
