#include "pdz_internal.h"

#include <quirewise/crypto.h>

#include <string.h>

void pdz_md5_digest(const void *data, size_t size, uint8_t out[16])
{
    if (!out) return;
    if (qw_crypto_md5_digest(data, size, out) != QW_CRYPTO_OK)
        memset(out, 0, 16u);
}

void pdz_md5_hex_upper(const void *data, size_t size, char out[33])
{
    static const char digits[] = "0123456789ABCDEF";
    uint8_t digest[16];

    if (!out) return;
    pdz_md5_digest(data, size, digest);
    for (size_t i = 0; i < sizeof(digest); ++i) {
        out[2u * i] = digits[digest[i] >> 4];
        out[2u * i + 1u] = digits[digest[i] & 0x0fu];
    }
    out[32] = '\0';
    qw_crypto_secure_zero(digest, sizeof(digest));
}

void pdz_aes256_decrypt_block(const uint8_t in[16], const uint8_t key[32],
                              uint8_t out[16])
{
    if (!in || !key || !out) return;
    memcpy(out, in, 16u);
    (void)qw_crypto_aes256_ecb_decrypt(key, out, 16u);
}
