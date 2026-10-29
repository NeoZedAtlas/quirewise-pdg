#include "pdz_internal.h"

#include <quirewise/crypto.h>

void pdz_word_swap32(uint8_t *data, size_t size)
{
    if (!data) return;
    for (size_t i = 0; size - i >= 4u; i += 4u) {
        uint8_t t0 = data[i + 0u];
        uint8_t t1 = data[i + 1u];
        data[i + 0u] = data[i + 3u];
        data[i + 1u] = data[i + 2u];
        data[i + 2u] = t1;
        data[i + 3u] = t0;
    }
}

void pdz_blowfish_ecb_decrypt(uint8_t *data, size_t size, const uint8_t key[16])
{
    const size_t usable = size - (size % 8u);
    QwCryptoStatus status;

    if (!data || !key || usable < 0x20u) return;

    /* PDZ swaps each 32-bit word around standard Blowfish ECB. */
    pdz_word_swap32(data, usable);
    status = qw_crypto_blowfish_ecb_decrypt(key, 16u, data, usable);
    if (status != QW_CRYPTO_OK) {
        pdz_word_swap32(data, usable);
        return;
    }
    pdz_word_swap32(data, usable);
}
