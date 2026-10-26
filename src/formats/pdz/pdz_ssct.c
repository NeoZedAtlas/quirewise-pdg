/*
 * Offline SSCT certificate decoding (pdgDll.dll family).
 *
 * Reversed from pdgDll.dll (Ghidra, ImageBase 0x10000000) and verified against
 * the engine on five real samples:
 *
 *   FUN_100c4b20  "SSCT" <text> "SSCE";  text = <hex ciphertext> "[p?" <params> "]"
 *   FUN_100051d0  data = text[:-32], digest = text[-32:]
 *                 assert MD5(data + "*s{P2a_1mP^?") == digest   (uppercase hex)
 *   FUN_10004650  key schedule:
 *                     out += key[i]
 *                     out += ALPHABET[(int8)tail6[i % 4] * i % 96]
 *                 padded to 32 chars with ALPHABET[0..]; hex-encoded
 *   FUN_10004bf0  AES-256-CBC over hex(data[:n-6] + data[n-2:n]) with
 *                 IV = hex(effective[n-36 : n-4])
 *
 * The 94-byte ALPHABET lives at pdgDll!DAT_101032a8.  A hand transcription with
 * two extra spaces (96 bytes) used to break 2 of the 12 key-schedule indices --
 * the bytes below are taken verbatim from the DLL.
 */

#include "pdz_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char pdz_ssct_alphabet[95] =
    "4+RTe6i#r3%9Cucv^&*()_bn2!yhj8'zI./QWE5=~tFmop[]\\a@qwBsg,OG$fkl;0-HJKNM<`1DdYUV7L:\"ZXxP{}|AS>?";

const char pdz_ssct_hdd_alphabet[95] =
    ")3#*E6i#r3%9Cucv^&*()_bn2!yhj8'zI./QWE5=~tFmop[]\\a@qwBsg,OG$fkl;0-HJKNM<`1DdYUV7L_\"ZXxP{}|8e<]";

/*!
 * MD5("#$FVU&T)(*&^$#") XOR C -- the fixed fallback member key.
 *
 * The engine tries the certificate bookkey first and this key second; it is
 * the only key that unlocks 01.pdz / 03.pdz. pdz_member_payload_ex() now
 * tries it too (see pdz_container.c).
 */
const uint8_t pdz_member_key_fixed[16] = {
    0x7a, 0xeb, 0x34, 0x9a, 0xb9, 0x77, 0xd2, 0x17,
    0xb9, 0x61, 0xaf, 0x98, 0x6a, 0x32, 0xbd, 0xdb};

#define PDZ_SSCT_ALPHABET_LEN 94

/*
 * Both engine tables are really 96 bytes: the 94 characters above followed by
 * two NULs, indexed with ``% 0x60``.  Indexing the 94-character string directly
 * would wrap instead of reading a NUL.
 */
static uint8_t alphabet96(const char *table, int index)
{
    return (uint8_t)((index >= 0 && index < 96 && index < 94) ? table[index] : 0);
}

/*
 * The shared body of pdgDll!FUN_10004650 and !FUN_10005830::
 *
 *     j = (int8)separator[i % 4] * i % 96
 *     acc += key[i] + ALPHABET[j]   when j < 94
 *     acc += key[i]                 when j >= 94  (the table byte is NUL)
 *
 * padded to 32 characters with ALPHABET[0..].
 */
static void interleave(const char *key, const char *separator, const char *table,
                       char out[33])
{
    size_t n = 0;
    size_t i;
    int pad = 0;

    for (i = 0; key[i] && n + 1 < 33; i++) {
        int v = (int)(signed char)separator[i % 4];
        int j = (v * (int)i) % 96;
        if (j < 0) {
            j += 96;
        }
        out[n++] = key[i];
        if (j < 0x5E && n + 1 < 33) {
            out[n++] = (char)alphabet96(table, j);
        }
    }
    while (n < 32) {
        out[n++] = (char)alphabet96(table, pad % 96);
        pad++;
    }
    out[32] = 0;
}

static const uint8_t *pdz_memmem(const uint8_t *hay, size_t hay_len,
                                const char *needle, size_t needle_len)
{
    size_t i;
    if (needle_len == 0 || hay_len < needle_len) {
        return NULL;
    }
    for (i = 0; i + needle_len <= hay_len; i++) {
        if (memcmp(hay + i, needle, needle_len) == 0) {
            return hay + i;
        }
    }
    return NULL;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

int pdz_ssct_expand_key(const char *key, const char *tail, size_t tail_len, uint8_t out[32])
{
    char text[33];

    if (!key || !tail || !out) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    (void)tail_len;
    interleave(key, tail, pdz_ssct_alphabet, text);
    memcpy(out, text, 32);
    return PDG_OK;
}

int pdz_ssct_hdd2key(const char *hddinfo, char *out, size_t out_size)
{
    static const char digits[] = "0123456789ABCDEF";
    char text[33];
    size_t i;
    size_t n = 0;

    if (!hddinfo || !out || out_size == 0) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    interleave(hddinfo, PDZ_SSCT_SEPARATOR, pdz_ssct_hdd_alphabet, text);
    out[0] = 0;
    for (i = 0; i < 32; i++) {
        unsigned c = (unsigned char)text[i];
        /* the engine uses ToString("X"): no leading zero */
        if (c >= 0x10) {
            if (n + 2 >= out_size) {
                return PDG_ERR_INVALID_ARGUMENT;
            }
            out[n++] = digits[c >> 4];
            out[n++] = digits[c & 15];
        } else {
            if (n + 1 >= out_size) {
                return PDG_ERR_INVALID_ARGUMENT;
            }
            out[n++] = digits[c & 15];
        }
    }
    out[n] = 0;
    return PDG_OK;
}

int pdz_ssct_decode_key64(const void *blob, size_t size, const char *key64,
                          uint8_t **out_xml, size_t *out_xml_len)
{
    const uint8_t *b = (const uint8_t *)blob;
    const char *text;
    size_t text_len, marker = (size_t)-1, end, n, i;
    char salt[5];
    char acc[33];
    uint8_t aes_key[32];
    uint8_t iv[16];
    uint8_t *cipher = NULL;
    uint8_t *plain = NULL;
    size_t ct_len;
    int rc;

    if (!b || !key64 || !out_xml || !out_xml_len || size < 16) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    if (memcmp(b, "SSCT", 4) != 0) {
        return PDG_ERR_NOT_PDG;
    }
    text = (const char *)(b + 4);
    text_len = size - 4;
    end = 0;
    for (i = 0; i + 4 <= text_len; i++) {
        if (memcmp(text + i, "SSCE", 4) == 0) {
            end = i;
            break;
        }
    }
    text_len = end ? end : text_len;
    for (i = 0; i + 3 <= text_len; i++) {
        if (memcmp(text + i, "[p?", 3) == 0) {
            marker = i;
            break;
        }
    }
    if (marker == (size_t)-1) {
        marker = text_len;
    }
    n = marker;
    if (n < 0x44) {
        return PDG_ERR_FORMAT;
    }
    memcpy(salt, text + (n - 0x26), 4);
    salt[4] = 0;

    interleave(key64, salt, pdz_ssct_alphabet, acc);
    memcpy(aes_key, acc, 32);

    for (i = 0; i < 16; i++) {
        int hi = hexval(text[n - 0x44 + 2 * i]);
        int lo = hexval(text[n - 0x44 + 2 * i + 1]);
        if (i == 15) { /* the last IV byte sits after the salt */
            hi = hexval(text[n - 0x22]);
            lo = hexval(text[n - 0x21]);
        }
        if (hi < 0 || lo < 0) {
            return PDG_ERR_FORMAT;
        }
        iv[i] = (uint8_t)((hi << 4) | lo);
    }
    ct_len = (n - 0x44) / 2;
    if ((n - 0x44) % 2 || ct_len == 0 || ct_len % 16) {
        return PDG_ERR_FORMAT;
    }
    cipher = (uint8_t *)malloc(ct_len);
    plain = (uint8_t *)malloc(ct_len + 1);
    if (!cipher || !plain) {
        rc = PDG_ERR_OUT_OF_MEMORY;
        goto done;
    }
    for (i = 0; i < ct_len; i++) {
        int hi = hexval(text[2 * i]);
        int lo = hexval(text[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            rc = PDG_ERR_FORMAT;
            goto done;
        }
        cipher[i] = (uint8_t)((hi << 4) | lo);
    }
    {
        uint8_t prev[16];
        memcpy(prev, iv, 16);
        for (i = 0; i < ct_len; i += 16) {
            uint8_t dec[16];
            size_t k;
            pdz_aes256_decrypt_block(cipher + i, aes_key, dec);
            for (k = 0; k < 16; k++) {
                plain[i + k] = (uint8_t)(dec[k] ^ prev[k]);
                prev[k] = cipher[i + k];
            }
        }
    }
    plain[ct_len] = 0;
    *out_xml = plain;
    *out_xml_len = ct_len;
    plain = NULL;
    rc = PDG_OK;

done:
    free(cipher);
    free(plain);
    return rc;
}

int pdz_ssct_solve_type2(const void *blob, size_t size, char *out_key64,
                         size_t key64_size, uint8_t **out_xml, size_t *out_xml_len)
{
    char hdd[16];
    char key64[96];
    unsigned value;
    int variant;

    if (!blob || !out_key64 || !out_xml || !out_xml_len) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    /* DownPdgByss!FUN_1001e530 builds the machine id as
     * sprintf("%ld", disk-derived-u32), so it can be NEGATIVE; and only its
     * first four characters survive the key schedule.  Sweep every shape that
     * signed decimal form can take (~12k candidates). */
    for (variant = 0; variant < 7; variant++) {
    unsigned limit = (variant == 0) ? 10000u
                   : (variant == 1) ? 1000u
                   : (variant == 2) ? 100u
                   : (variant == 3) ? 10u
                   : (variant == 4) ? 1000u
                   : (variant == 5) ? 100u : 10u;
    unsigned start = (variant == 4) ? 0u : (variant >= 5 ? 10u : 0u);
    for (value = start; value < limit; value++) {
        uint8_t *xml = NULL;
        size_t xml_len = 0;
        int rc;

        switch (variant) {
        case 0: snprintf(hdd, sizeof(hdd), "%04u", value); break;
        case 1: snprintf(hdd, sizeof(hdd), "%u", value); break;
        case 2: snprintf(hdd, sizeof(hdd), "%u", value); break;
        case 3: snprintf(hdd, sizeof(hdd), "%u", value); break;
        case 4: snprintf(hdd, sizeof(hdd), "-%03u", value); break;
        case 5: snprintf(hdd, sizeof(hdd), "-%u", value); break;
        default: snprintf(hdd, sizeof(hdd), "-%u", value); break;
        }
        rc = pdz_ssct_hdd2key(hdd, key64, sizeof(key64));
        if (rc != PDG_OK) {
            return rc;
        }
        rc = pdz_ssct_decode_key64(blob, size, key64, &xml, &xml_len);
        /* A three-byte test has a ~1/16.7M false-positive rate, which is not
         * negligible across a 10^7 candidate sweep; require the whole shape. */
        if (rc == PDG_OK && xml_len >= 19 &&
            memcmp(xml, "<?xml version=\"1.0\"", 19) == 0 &&
            pdz_memmem(xml, xml_len, "</cert>", 7) != NULL &&
            pdz_memmem(xml, xml_len, "<bookkey>", 9) != NULL) {
            if (strlen(key64) + 1 > key64_size) {
                free(xml);
                return PDG_ERR_INVALID_ARGUMENT;
            }
            strcpy(out_key64, key64);
            *out_xml = xml;
            *out_xml_len = xml_len;
            return PDG_OK;
        }
        free(xml);
    }
    }
    return PDG_ERR_FORMAT;   /* no candidate produced a certificate */
}


/* 1 when the params select the certificate type the engine also rejects. */
static int ssct_rejected(const char *params, size_t len)
{
    static const char t2[] = "type=2";
    static const char un[] = "un=unRegister";
    size_t i;
    int has_t2 = 0, has_un = 0;

    for (i = 0; i + sizeof(t2) - 1 <= len; i++) {
        if (memcmp(params + i, t2, sizeof(t2) - 1) == 0) {
            has_t2 = 1;
        }
    }
    for (i = 0; i + sizeof(un) - 1 <= len; i++) {
        if (memcmp(params + i, un, sizeof(un) - 1) == 0) {
            has_un = 1;
        }
    }
    return has_t2 && has_un;
}

int pdz_ssct_decode(const void *blob, size_t size, uint8_t **out_xml, size_t *out_xml_len)
{
    const uint8_t *b = (const uint8_t *)blob;
    const char *text;
    size_t text_len, marker = (size_t)-1, end, n, i;
    char params[128];
    size_t params_len;
    const char *data;
    const char *digest;
    uint8_t key[32];
    uint8_t iv[16];
    uint8_t *effective = NULL;
    uint8_t *cipher = NULL;
    uint8_t *plain = NULL;
    char want[33];
    size_t ct_len;
    int rc = PDG_OK;

    if (!b || !out_xml || !out_xml_len || size < 16) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    if (memcmp(b, "SSCT", 4) != 0) {
        return PDG_ERR_NOT_PDG;
    }
    text = (const char *)(b + 4);
    text_len = size - 4;
    end = 0;
    for (i = 0; i + 4 <= text_len; i++) {
        if (memcmp(text + i, "SSCE", 4) == 0) {
            end = i;
            break;
        }
    }
    if (end == 0) {
        return PDG_ERR_FORMAT;
    }
    text_len = end;
    for (i = 0; i + 3 <= text_len; i++) {
        if (memcmp(text + i, "[p?", 3) == 0) {
            marker = i;
            break;
        }
    }
    if (marker == (size_t)-1 || marker < 32) {
        return PDG_ERR_FORMAT;
    }
    {
        size_t close = marker;
        while (close < text_len && text[close] != ']') {
            close++;
        }
        params_len = close - (marker + 3);
        if (params_len >= sizeof(params)) {
            params_len = sizeof(params) - 1;
        }
        memcpy(params, text + marker + 3, params_len);
        params[params_len] = 0;
    }
    if (ssct_rejected(params, params_len)) {
        return PDG_ERR_UNSUPPORTED;
    }

    data = text;
    n = marker - 32;            /* data length; the last 32 chars are the digest */
    digest = text + n;

    /* checksum: MD5(data + hardcoded key), uppercase hex */
    {
        char *joined = (char *)malloc(n + 32);
        if (!joined) {
            return PDG_ERR_OUT_OF_MEMORY;
        }
        memcpy(joined, data, n);
        memcpy(joined + n, PDZ_SSCT_HARDCODED_KEY, sizeof(PDZ_SSCT_HARDCODED_KEY) - 1);
        pdz_md5_hex_upper(joined, n + sizeof(PDZ_SSCT_HARDCODED_KEY) - 1, want);
        free(joined);
    }
    if (memcmp(want, digest, 32) != 0) {
        return PDG_ERR_FORMAT;   /* checksum mismatch (debug: distinct from magic) */
    }

    /* engine quirk: bytes n-6 / n-5 are rewritten with data[n-2] / data[n-1] */
    effective = (uint8_t *)malloc(n);
    if (!effective) {
        return PDG_ERR_OUT_OF_MEMORY;
    }
    memcpy(effective, data, n - 6);
    effective[n - 6] = (uint8_t)data[n - 2];
    effective[n - 5] = (uint8_t)data[n - 1];

    rc = pdz_ssct_expand_key(PDZ_SSCT_HARDCODED_KEY, data + (n - 6), 6, key);
    if (rc != PDG_OK) {
        goto done;
    }
    for (i = 0; i < 16; i++) {
        int hi = hexval(effective[n - 36 + 2 * i]);
        int lo = hexval(effective[n - 36 + 2 * i + 1]);
        if (hi < 0 || lo < 0) {
            rc = PDG_ERR_FORMAT;
            goto done;
        }
        iv[i] = (uint8_t)((hi << 4) | lo);
    }
    ct_len = (n - 4) / 2;
    if ((n - 4) % 2 || ct_len == 0) {
        rc = PDG_ERR_FORMAT;
        goto done;
    }
    cipher = (uint8_t *)malloc(ct_len);
    plain = (uint8_t *)malloc(ct_len + 1);
    if (!cipher || !plain) {
        rc = PDG_ERR_OUT_OF_MEMORY;
        goto done;
    }
    for (i = 0; i < ct_len; i++) {
        int hi = hexval(effective[2 * i]);
        int lo = hexval(effective[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            rc = PDG_ERR_FORMAT;
            goto done;
        }
        cipher[i] = (uint8_t)((hi << 4) | lo);
    }
    if (ct_len % 16) {
        rc = PDG_ERR_FORMAT;
        goto done;
    }
    for (i = 0; i < ct_len; i += 16) {
        uint8_t dec[16];
        size_t k;
        pdz_aes256_decrypt_block(cipher + i, key, dec);
        for (k = 0; k < 16; k++) {
            plain[i + k] = (uint8_t)(dec[k] ^ iv[k]);
            iv[k] = cipher[i + k];
        }
    }
    plain[ct_len] = 0;
    *out_xml = plain;
    *out_xml_len = ct_len;
    plain = NULL;

done:
    free(effective);
    free(cipher);
    free(plain);
    return rc;
}

int pdz_ssct_bookkey(const void *blob, size_t size, char *out, size_t out_size)
{
    uint8_t *xml = NULL;
    size_t xml_len = 0;
    size_t i;
    int rc = pdz_ssct_decode(blob, size, &xml, &xml_len);

    if (rc != PDG_OK) {
        return rc;
    }
    if (!out || out_size == 0) {
        free(xml);
        return PDG_ERR_INVALID_ARGUMENT;
    }
    out[0] = 0;
    for (i = 0; i + 9 <= xml_len; i++) {
        if (memcmp(xml + i, "<bookkey>", 9) == 0) {
            size_t j = 0;
            i += 9;
            while (i + j < xml_len && xml[i + j] != '<' && j + 1 < out_size) {
                out[j] = (char)xml[i + j];
                j++;
            }
            out[j] = 0;
            free(xml);
            return j ? PDG_OK : PDG_ERR_FORMAT;
        }
    }
    free(xml);
    return PDG_ERR_FORMAT;
}

int pdz_ssct_bookkey_from_xml(const void *xml, size_t xml_len, char *out, size_t out_size)
{
    const uint8_t *p = (const uint8_t *)xml;
    size_t i;

    if (!p || !out || out_size == 0) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    out[0] = 0;
    for (i = 0; i + 9 <= xml_len; i++) {
        if (memcmp(p + i, "<bookkey>", 9) == 0) {
            size_t j = 0;
            i += 9;
            while (i + j < xml_len && p[i + j] != '<' && j + 1 < out_size) {
                out[j] = (char)p[i + j];
                j++;
            }
            out[j] = 0;
            return j ? PDG_OK : PDG_ERR_FORMAT;
        }
    }
    return PDG_ERR_FORMAT;
}
