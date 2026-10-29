#ifndef PDZ_INTERNAL_H
#define PDZ_INTERNAL_H

#include "pdg.h"
#include "pdz.h"

#include "../../core/byte_utils.h"

#include <stddef.h>
#include <stdint.h>

/* Hardcoded catalog key string. Identical on all five reference samples,
 * registered or not, and confirmed byte-for-byte inside the official reader. */
#define PDZ_CATALOG_KEY_STRING \
    "4D346125645E6543205C6240795C206A50346141634C6B6F655C742F505E6466"

/* Engine-verified key derivation constants (little-endian dwords). */
#define PDZ_KEY_CONST_0 0x00820208u
#define PDZ_KEY_CONST_1 0x01220208u
#define PDZ_KEY_CONST_2 0x00805038u
#define PDZ_KEY_CONST_3 0x00920208u

/* Format-specific MD5 text conversion and word swap wrappers. */
void pdz_tea128_decrypt(uint8_t *data, size_t size, const uint8_t key[16]);
void pdz_md5_digest(const void *data, size_t size, uint8_t out[16]);
void pdz_md5_hex_upper(const void *data, size_t size, char out[33]);

/* Reverse the bytes of each 32-bit word in place. */
void pdz_word_swap32(uint8_t *data, size_t size);

/* pdgDll 0x1000b780: Blowfish-ECB on byte-swapped 32-bit words, 8-byte blocks.
 * `size` is rounded down to a multiple of 8 and the call is a no-op below 32. */
void pdz_blowfish_ecb_decrypt(uint8_t *data, size_t size, const uint8_t key[16]);

void pdz_derive_key_from(const uint8_t *key_string, size_t length, uint8_t key16[16]);

#endif
