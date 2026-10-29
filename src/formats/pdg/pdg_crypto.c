#include "pdg_internal.h"

#include <string.h>

#include <quirewise/crypto.h>

static const uint8_t KEY_SUPERSTAR[16] = {
    'S', 'U', 'P', 'E', 'R', 'S', 'T', 'A', 'R', '4', 'P', 'D', 'G', '2', '.', '0'
};
static const uint8_t KEY_EFGF[8] = { 'e', '#', 'f', 'g', 'F', '%', '3', '*' };
static const uint8_t KEY_HJOP[16] = {
    'h', 'j', 'o', 'p', '2', '3', '9', '-', 'l', 'i', 'w', 'r', 's', 'd', 'k', 'o'
};
static const uint8_t KEY_PI[8] = { '3', '.', '1', '4', '1', '5', '9', '2' };
static const uint8_t KEY_SSREADER[8] = { 'S', 'S', 'R', 'E', 'A', 'D', 'E', 'R' };

void pdg_repeating_xor(uint8_t *data, size_t size, const uint8_t *key, size_t key_size)
{
    if (!data || !key || key_size == 0) {
        return;
    }
    for (size_t i = 0; i < size; ++i) {
        data[i] ^= key[i % key_size];
    }
}

void pdg_xor_pi(uint8_t *data, size_t size)
{
    if (!data) {
        return;
    }
    for (size_t i = 0; i < size; ++i) {
        data[i] ^= KEY_PI[i & 7];
    }
}

void pdg_pdg_block_decrypt(uint8_t block[16], const uint8_t key[16])
{
    if (block && key)
        (void)qw_crypto_pdg_tea128_ecb_decrypt(key, block, 16u);
}

void pdg_pdg_block_decrypt_buffer(uint8_t *data, size_t size, const uint8_t key[16])
{
    if (!data || !key) {
        return;
    }
    (void)qw_crypto_pdg_tea128_ecb_decrypt(key, data, size);
}

static void derive_md5_key(const PdgHeader *header, bool mode4_hjop, uint8_t key[16])
{
    uint8_t material[0x30];
    memcpy(material, header->key_data, sizeof(material));
    if (mode4_hjop && header->type == 4) {
        for (size_t i = 0; i < 12; ++i) {
            material[0x24 + i] ^= KEY_HJOP[i];
        }
    }
    if (qw_crypto_md5_digest(material, sizeof(material), key) != QW_CRYPTO_OK)
        memset(key, 0, 16u);
    qw_crypto_secure_zero(material, sizeof(material));
}

static void decrypt_with_primary_dispatch(uint8_t *payload, size_t payload_size, const PdgHeader *header)
{
    uint8_t key[16];
    derive_md5_key(header, true, key);

    if (header->type == 1) {
        pdg_xor_pi(payload, payload_size);
        qw_crypto_secure_zero(key, sizeof(key));
        return;
    }
    if (header->type == 5) {
        pdg_repeating_xor(key, 8, KEY_EFGF, sizeof(KEY_EFGF));
    } else if (header->type >= 3) {
        pdg_repeating_xor(key, 8, KEY_SUPERSTAR, 8);
    }
    pdg_pdg_block_decrypt_buffer(payload, payload_size, key);
    qw_crypto_secure_zero(key, sizeof(key));
}

static void decrypt_with_ssreader_variant(uint8_t *payload, size_t payload_size, const PdgHeader *header)
{
    uint8_t key[16];
    derive_md5_key(header, false, key);
    if (header->type >= 3) {
        pdg_repeating_xor(key, 8, KEY_SSREADER, sizeof(KEY_SSREADER));
    }
    pdg_pdg_block_decrypt_buffer(payload, payload_size, key);
    qw_crypto_secure_zero(key, sizeof(key));
}

int pdg_decrypt_payload_in_place(uint8_t *payload, size_t payload_size, const PdgHeader *header)
{
    if (!payload || !header) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    if (header->type == 0 || header->type == 0x10 || (header->type >= 0x11 && header->type <= 0x1c) ||
        header->type == 0xaa || header->type == 0xab || header->type == 0xac) {
        return PDG_OK;
    }
    if (header->type == 4) {
        decrypt_with_ssreader_variant(payload, payload_size, header);
    } else {
        decrypt_with_primary_dispatch(payload, payload_size, header);
    }
    return PDG_OK;
}
