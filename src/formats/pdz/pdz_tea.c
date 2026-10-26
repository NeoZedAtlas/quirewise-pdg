#include "pdz_internal.h"

/*
 * TEA-128, decryption direction, transcribed instruction-by-instruction from
 * pdgDll!0x1000ba40 (see PDZ_CONTAINER_VERIFIED.md 3.2).
 *
 * Differences from textbook TEA that matter:
 *   - 16 rounds, not 32
 *   - the round counter starts at 0xE3779B90 and counts *down* by 0x9E3779B9
 *   - four sub-blocks (v0..v3) per 16-byte block, not two
 */

void pdz_tea128_decrypt(uint8_t *data, size_t size, const uint8_t key[16])
{
    const uint32_t k0 = qw_read_u32_le(key + 0);
    const uint32_t k1 = qw_read_u32_le(key + 4);
    const uint32_t k2 = qw_read_u32_le(key + 8);
    const uint32_t k3 = qw_read_u32_le(key + 12);

    const size_t blocks = size / PDZ_TEA_BLOCK_SIZE;

    for (size_t i = 0; i < blocks; ++i) {
        uint8_t *block = data + i * PDZ_TEA_BLOCK_SIZE;
        uint32_t v0 = qw_read_u32_le(block + 0);
        uint32_t v1 = qw_read_u32_le(block + 4);
        uint32_t v2 = qw_read_u32_le(block + 8);
        uint32_t v3 = qw_read_u32_le(block + 12);
        uint32_t sum = PDZ_TEA_SUM_INIT;

        for (int round = 0; round < PDZ_TEA_ROUNDS; ++round) {
            v3 -= ((v0 << 4) + k2) ^ (v0 + sum) ^ ((v0 >> 5) + k1);
            v2 -= ((v3 << 4) + k0) ^ (v3 + sum) ^ ((v3 >> 5) + k3);
            v1 -= ((v2 << 4) + k2) ^ (v2 + sum) ^ ((v2 >> 5) + k3);
            v0 -= ((v1 << 4) + k0) ^ (v1 + sum) ^ ((v1 >> 5) + k1);
            sum -= PDZ_TEA_SUM_DELTA;
        }

        qw_write_u32_le(block + 0, v0);
        qw_write_u32_le(block + 4, v1);
        qw_write_u32_le(block + 8, v2);
        qw_write_u32_le(block + 12, v3);
    }
}
