#include "pdz_internal.h"

#include <stdlib.h>
#include <string.h>

/*
 * PDZ container layer. All primitives below are verified against the official
 * engine; see docs/authoritative/PDZ_CONTAINER_VERIFIED.md.
 *
 * Layout:
 *   [0x00 .. 0x60)              header
 *   [0x60 .. catalog_offset)    members, back to back, no gaps
 *   [catalog_offset .. +size)   member table, type-1 TEA-128
 *   [catalog_offset + size ..)  SSCT certificate area
 */

int pdz_abi_version(void)
{
    return PDZ_ABI_VERSION;
}

const char *pdz_cipher_name(uint8_t cipher_type)
{
    switch (cipher_type) {
    case 1:
        return "tea";
    case 2:
        return "des";
    case 3:
        return "unknown";
    case 4:
        return "blowfish";
    default:
        return "unrecognized";
    }
}

void pdz_derive_key_from(const uint8_t *key_string, size_t length, uint8_t key16[16])
{
    uint8_t digest[16];
    pdz_md5_digest(key_string, length, digest);

    const uint32_t constants[4] = {
        PDZ_KEY_CONST_0,
        PDZ_KEY_CONST_1,
        PDZ_KEY_CONST_2,
        PDZ_KEY_CONST_3,
    };
    for (int i = 0; i < 4; ++i) {
        uint32_t value = qw_read_u32_le(digest + i * 4) ^ constants[i];
        qw_write_u32_le(key16 + i * 4, value);
    }
}

int pdz_derive_key(const char *key_string, uint8_t key16[16])
{
    if (!key_string || !key16) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    pdz_derive_key_from((const uint8_t *)key_string, strlen(key_string), key16);
    return PDG_OK;
}

int pdz_is_container(const void *data, size_t size)
{
    if (!data || size < PDZ_HEADER_SIZE) {
        return 0;
    }
    return memcmp(data, "%pdgz", 5) == 0 ? 1 : 0;
}

void pdz_buffer_free(PdzBuffer *buffer)
{
    if (!buffer) {
        return;
    }
    free(buffer->data);
    buffer->data = NULL;
    buffer->size = 0;
    buffer->prefix_decrypted = 0;
    buffer->whole_decrypted = 0;
    buffer->used_fixed_key = 0;
    buffer->stored_as_is = 0;
}

const char *pdz_member_scope_name(int scope)
{
    switch (scope) {
    case PDZ_MEMBER_SCOPE_AUTO:
        return "auto";
    case PDZ_MEMBER_SCOPE_PREFIX:
        return "prefix(1024)";
    case PDZ_MEMBER_SCOPE_WHOLE:
        return "whole";
    default:
        return "unrecognized";
    }
}

int pdz_member_scope_for(const void *data, size_t size)
{
    if (!data || size < PDZ_HEADER_SIZE) {
        return PDZ_MEMBER_SCOPE_PREFIX;
    }
    const uint8_t *bytes = (const uint8_t *)data;
    if (memcmp(bytes, "%pdgz", 5) != 0) {
        return PDZ_MEMBER_SCOPE_PREFIX;
    }
    /* CORRECTED 2026-10-15: bookType 2 does NOT mean "whole member".
     *
     * 96304257.pdz has bookType == 2, and the engine's own member bytes satisfy
     * `engine[1024:] == stored[1024:]` byte-for-byte for all seven members.
     * Decoding with a 1024-byte prefix reproduces the official PDFs exactly,
     * while whole-member decryption yields garbage past offset 1024 (the first
     * visible symptom is that the 0x1A zlib stream dies at input byte ~1117).
     *
     * The real selector is therefore still unknown; the safe default is the
     * 1024-byte prefix, and callers may pass an explicit scope. */
    (void)bytes;
    return PDZ_MEMBER_SCOPE_PREFIX;
}

/*
 * Decrypt the member table into a freshly allocated buffer. Returns PDG_OK and
 * hands ownership to the caller.
 */
static int decrypt_catalog(const uint8_t *data, size_t size, uint32_t catalog_offset,
                           uint32_t catalog_size, uint8_t **plain_out, size_t *plain_size_out)
{
    if (catalog_size == 0) {
        return PDG_ERR_EMPTY;
    }
    if ((size_t)catalog_offset + catalog_size > size) {
        return PDG_ERR_FORMAT;
    }

    uint8_t *plain = (uint8_t *)malloc(catalog_size);
    if (!plain) {
        return PDG_ERR_OUT_OF_MEMORY;
    }
    memcpy(plain, data + catalog_offset, catalog_size);

    uint8_t key[16];
    pdz_derive_key_from((const uint8_t *)PDZ_CATALOG_KEY_STRING, strlen(PDZ_CATALOG_KEY_STRING), key);
    pdz_tea128_decrypt(plain, catalog_size, key);

    *plain_out = plain;
    *plain_size_out = catalog_size;
    return PDG_OK;
}

/* A record is only accepted when its name is printable ASCII and its range
 * stays inside the file, matching the prototype's sanity checks. */
static int record_is_plausible(const uint8_t *record, size_t file_size, PdzMember *out)
{
    char name[PDZ_NAME_SIZE];
    memset(name, 0, sizeof(name));

    size_t name_length = 0;
    while (name_length < 12 && record[name_length] != 0) {
        ++name_length;
    }
    if (name_length == 0) {
        return 0;
    }
    for (size_t i = 0; i < name_length; ++i) {
        if (record[i] < 32 || record[i] >= 127) {
            return 0;
        }
    }
    memcpy(name, record, name_length);

    uint32_t offset = qw_read_u32_le(record + 12);
    uint32_t size = qw_read_u32_le(record + 16);
    if (offset == 0 || offset >= file_size) {
        return 0;
    }
    if (size == 0 || size > file_size - offset) {
        return 0;
    }

    memcpy(out->name, name, sizeof(name));
    out->offset = offset;
    out->size = size;
    return 1;
}

int pdz_members(const void *data,
                size_t size,
                PdzMember *members,
                size_t capacity,
                size_t *count)
{
    if (!data || !count) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    if (size < PDZ_HEADER_SIZE) {
        return PDG_ERR_EMPTY;
    }
    const uint8_t *bytes = (const uint8_t *)data;
    if (memcmp(bytes, "%pdgz", 5) != 0) {
        return PDG_ERR_NOT_PDZ;
    }

    const uint32_t catalog_offset = qw_read_u32_le(bytes + 0x14);
    const uint32_t catalog_size = qw_read_u32_le(bytes + 0x18);

    uint8_t *plain = NULL;
    size_t plain_size = 0;
    int rc = decrypt_catalog(bytes, size, catalog_offset, catalog_size, &plain, &plain_size);
    if (rc != PDG_OK) {
        return rc;
    }

    size_t found = 0;
    const size_t records = plain_size / PDZ_RECORD_SIZE;
    for (size_t i = 0; i < records; ++i) {
        PdzMember candidate;
        if (!record_is_plausible(plain + i * PDZ_RECORD_SIZE, (size_t)size, &candidate)) {
            continue;
        }
        if (members && found < capacity) {
            members[found] = candidate;
        }
        ++found;
    }

    free(plain);
    *count = found;
    return PDG_OK;
}

int pdz_parse(const void *data, size_t size, PdzInfo *info)
{
    if (!data || !info) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    if (size < PDZ_HEADER_SIZE) {
        return PDG_ERR_EMPTY;
    }
    const uint8_t *bytes = (const uint8_t *)data;
    if (memcmp(bytes, "%pdgz", 5) != 0) {
        return PDG_ERR_NOT_PDZ;
    }
    if (size > 0xFFFFFFFFu) {
        return PDG_ERR_FORMAT;
    }

    memset(info, 0, sizeof(*info));
    memcpy(info->version, bytes + 8, 4);
    info->version[4] = '\0';
    info->cipher_type = bytes[0x0D];
    info->book_type = bytes[0x0F];
    info->catalog_offset = qw_read_u32_le(bytes + 0x14);
    info->catalog_size = qw_read_u32_le(bytes + 0x18);
    memcpy(info->producer, bytes + 0x1C, 18);
    info->producer[18] = '\0';
    info->file_size = (uint32_t)size;

    /* Verified invariant: catalog_offset + catalog_size is exactly where the
     * SSCT marker begins. Confirm the literal is there rather than assuming. */
    const size_t ssct = (size_t)info->catalog_offset + info->catalog_size;
    if (ssct + 4 <= size && memcmp(bytes + ssct, "SSCT", 4) == 0) {
        info->ssct_offset = (uint32_t)ssct;
    }

    size_t member_count = 0;
    int rc = pdz_members(bytes, size, NULL, 0, &member_count);
    if (rc == PDG_OK) {
        info->member_count = (uint32_t)member_count;
    }

    return PDG_OK;
}

int pdz_validate(const void *data,
                 size_t size,
                 const PdzMember *members,
                 size_t count,
                 PdzValidation *out)
{
    if (!data || !out) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    if (size < PDZ_HEADER_SIZE) {
        return PDG_ERR_EMPTY;
    }
    const uint8_t *bytes = (const uint8_t *)data;
    if (memcmp(bytes, "%pdgz", 5) != 0) {
        return PDG_ERR_NOT_PDZ;
    }

    memset(out, 0, sizeof(*out));

    const uint32_t catalog_offset = qw_read_u32_le(bytes + 0x14);
    const uint32_t catalog_size = qw_read_u32_le(bytes + 0x18);
    const size_t expected_ssct = (size_t)catalog_offset + catalog_size;
    out->catalog_offset_consistent =
        (expected_ssct + 4 <= size && memcmp(bytes + expected_ssct, "SSCT", 4) == 0) ? 1 : 0;

    /* The members must tile [0x60, catalog_offset) with no gaps and no
     * overlap; that is the strongest single check that the table decrypted
     * correctly rather than merely producing plausible-looking records. */
    uint32_t cursor = PDZ_HEADER_SIZE;
    uint32_t gaps = 0;
    for (size_t i = 0; i < count; ++i) {
        if (members[i].offset != cursor) {
            ++gaps;
        }
        cursor = members[i].offset + members[i].size;
    }
    out->tiling_end = cursor;
    out->tiling_gaps = gaps;
    out->members_tile = (gaps == 0 && count > 0 && cursor == catalog_offset) ? 1 : 0;

    return PDG_OK;
}

/*
 * Does a decrypted member head look like a known plaintext?
 *
 * Used only to choose between the two candidate member keys (certificate
 * bookkey vs the fixed fallback), so the hit criterion can be strict: with two
 * candidates a 1-in-65536 two-byte coincidence is harmless, whereas the old
 * 3-byte `<?x` certificate test failed at 10^7 scale (see PITFALLS M1).  The
 * PDG branch additionally requires a known pdg_type byte, which makes the
 * container case effectively collision-free.
 */
static int pdz_looks_like_member_plaintext(const uint8_t *blob, size_t len)
{
    if (!blob || len < 4) {
        return 0;
    }
    if (blob[0] == 'H' && blob[1] == 'H' && len > 0x0F) {
        const uint8_t type = blob[0x0F];
        if ((type <= 0x05) || (type >= 0x10 && type <= 0x1C) ||
            (type == 0x28) || (type >= 0xAA && type <= 0xAC)) {
            return 1;
        }
    }
    if (len >= 3 && blob[0] == 0xFF && blob[1] == 0xD8 && blob[2] == 0xFF) {
        return 1; /* JPEG */
    }
    if (len >= 8 && memcmp(blob, "\x89PNG\r\n\x1a\n", 8) == 0) {
        return 1;
    }
    if (memcmp(blob, "%PDF", 4) == 0) {
        return 1;
    }
    if (memcmp(blob, "AT&T", 4) == 0) {
        return 1; /* DjVu */
    }
    if (memcmp(blob, "II*\0", 4) == 0 || memcmp(blob, "MM\0*", 4) == 0) {
        return 1; /* TIFF */
    }
    if ((len >= 5 && memcmp(blob, "<?xml", 5) == 0) ||
        memcmp(blob, "PK\x03\x04", 4) == 0) {
        return 1;
    }
    if (memcmp(blob, "BKRT", 4) == 0) {
        return 1; /* inforule.dat rule table */
    }
    if (len >= 6 && (memcmp(blob, "GIF87a", 6) == 0 || memcmp(blob, "GIF89a", 6) == 0)) {
        return 1;
    }
    return 0;
}

int pdz_member_payload_ex(const void *data,
                          size_t size,
                          const PdzMember *member,
                          const char *key_string,
                          int scope,
                          PdzBuffer *out)
{
    if (!data || !member || !out) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    if ((size_t)member->offset + member->size > size) {
        return PDG_ERR_FORMAT;
    }

    uint8_t *copy = (uint8_t *)malloc(member->size ? member->size : 1);
    if (!copy) {
        return PDG_ERR_OUT_OF_MEMORY;
    }
    memcpy(copy, (const uint8_t *)data + member->offset, member->size);

    if (scope == PDZ_MEMBER_SCOPE_AUTO) {
        scope = pdz_member_scope_for(data, size);
    }
    const int whole = (scope == PDZ_MEMBER_SCOPE_WHOLE);
    const size_t span = (whole || member->size < PDZ_MEMBER_PREFIX_SIZE)
                            ? member->size
                            : PDZ_MEMBER_PREFIX_SIZE;

    /* Members are not uniformly encrypted: 96304257.pdz stores inforule.dat
     * (BKRT), cover.jpg and meta.xml in clear next to its seven Blowfish'd
     * page members. Decrypting those would destroy them, so an already
     * plaintext member is returned exactly as stored. */
    if (pdz_looks_like_member_plaintext(copy, span)) {
        out->stored_as_is = 1;
        out->data = copy;
        out->size = member->size;
        return PDG_OK;
    }

    if (key_string) {
        uint8_t key[16];
        pdz_derive_key_from((const uint8_t *)key_string, strlen(key_string), key);
        if (whole && member->size > 0) {
            pdz_blowfish_ecb_decrypt(copy, member->size, key);
            out->whole_decrypted = 1;
        } else if (member->size >= PDZ_MEMBER_PREFIX_SIZE) {
            pdz_blowfish_ecb_decrypt(copy, PDZ_MEMBER_PREFIX_SIZE, key);
            out->prefix_decrypted = 1;
        }
    }

    /* Engine order: certificate key first, then the fixed fallback key.  Only
     * switch when the first candidate looks wrong and the fallback looks
     * right, so a wrong bookkey never survives into the output and a wrong
     * fallback never replaces a good bookkey result.  When neither looks
     * right (04.pdz) the requested key's result is returned unchanged. */
    if (!pdz_looks_like_member_plaintext(copy, span)) {
        uint8_t *trial = (uint8_t *)malloc(member->size ? member->size : 1);
        if (trial) {
            memcpy(trial, (const uint8_t *)data + member->offset, member->size);
            if (whole) {
                pdz_blowfish_ecb_decrypt(trial, member->size, pdz_member_key_fixed);
            } else if (member->size >= PDZ_MEMBER_PREFIX_SIZE) {
                pdz_blowfish_ecb_decrypt(trial, PDZ_MEMBER_PREFIX_SIZE, pdz_member_key_fixed);
            }
            if (pdz_looks_like_member_plaintext(trial, span)) {
                free(copy);
                copy = trial;
                out->used_fixed_key = 1;
                out->prefix_decrypted = 0;
                out->whole_decrypted = 0;
                if (whole) {
                    out->whole_decrypted = 1;
                } else {
                    out->prefix_decrypted = 1;
                }
            } else {
                free(trial);
            }
        }
    }

    out->data = copy;
    out->size = member->size;
    return PDG_OK;
}

int pdz_member_payload(const void *data,
                       size_t size,
                       const PdzMember *member,
                       const char *key_string,
                       PdzBuffer *out)
{
    return pdz_member_payload_ex(data, size, member, key_string, PDZ_MEMBER_SCOPE_AUTO, out);
}
