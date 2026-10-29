#include "pdz_internal.h"

#include <quirewise/crypto.h>

void pdz_tea128_decrypt(uint8_t *data, size_t size, const uint8_t key[16])
{
    (void)qw_crypto_pdg_tea128_ecb_decrypt(key, data, size);
}
