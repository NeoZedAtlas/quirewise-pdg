#ifndef PDZ_H
#define PDZ_H

/*
 * PDZ container support.
 *
 * A `.pdz` is PacketPdg's multi-page container: a 0x60-byte header, a run of
 * members laid out back to back, an encrypted member table ("catalog"), and a
 * trailing SSCT certificate area.
 *
 * Everything in this header is transcribed from primitives that were verified
 * byte-for-byte against the official engine (pdgDll.dll) by live observation;
 * see docs/authoritative/PDZ_CONTAINER_VERIFIED.md. Nothing here is guessed.
 *
 *   catalog : type-1 TEA-128, key_string is a hardcoded constant
 *   member  : type-4 Blowfish-ECB; key_string is the certificate's <bookkey>.
 *             How much of the member is encrypted depends on the container's
 *             bookType byte -- see PdzMemberScope. Pass NULL to leave members
 *             as stored.
 *   both    : key16 = MD5(key_string) XOR C[4], and the engine's Blowfish
 *             operates on byte-swapped 32-bit words.
 *
 * A PDZ whose members carry pdg_type 0x1A is not a bag of single pages: the
 * per-page split lives in a sibling `inforule.dat` (BKRT) rule file that the
 * reader loads through pdfpdgdll.dll. See docs/authoritative/PDG_TYPE_1A.md.
 */

#include "pdg.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PDZ_ABI_VERSION 4

#define PDZ_HEADER_SIZE 0x60
#define PDZ_RECORD_SIZE 20
#define PDZ_NAME_SIZE 13
#define PDZ_MEMBER_PREFIX_SIZE 1024

/*! Parsed container header. */
typedef struct PdzInfo {
    char version[5];      /*!< e.g. "1.01" */
    char producer[19];    /*!< e.g. "Made by PacketPdg" */
    uint8_t cipher_type;  /*!< header +0x0D; 4 = Blowfish in every known sample */
    uint8_t book_type;    /*!< header +0x0F; 1 or 2 */
    uint32_t catalog_offset;
    uint32_t catalog_size;
    uint32_t member_count; /*!< records recovered from the decrypted catalog */
    uint32_t ssct_offset;  /*!< 0 when the catalog invariant does not hold */
    uint32_t file_size;
} PdzInfo;

/*! One entry of the decrypted member table. */
typedef struct PdzMember {
    char name[PDZ_NAME_SIZE];
    uint32_t offset;
    uint32_t size;
} PdzMember;

/*! Owned byte buffer returned by the PDZ API. */
typedef struct PdzBuffer {
    uint8_t *data;
    size_t size;
    int prefix_decrypted; /*!< 1 when a key_string was applied to the prefix */
    int whole_decrypted;  /*!< 1 when a key_string was applied to the whole member */
    int used_fixed_key;   /*!< 1 when the fixed fallback key beat the bookkey (ABI 3) */
    int stored_as_is;     /*!< 1 when the member was already plaintext and kept verbatim (ABI 3) */
} PdzBuffer;

/*!
 * How much of a member is Blowfish-ECB encrypted.
 *
 * CORRECTED 2026-10-15: the earlier "bookType == 2 -> whole member,
 * bookType == 1 -> prefix" mapping was WRONG and is not used.
 *
 *   bookType == 2   ->  PREFIX (1024)
 *       96304257.pdz has bookType == 2, yet its members satisfy
 *       `engine[1024:] == stored[1024:]` byte for byte, and decoding with a
 *       1024-byte prefix reproduces the official PDFs exactly; whole-member
 *       decryption yields garbage past offset 1024 (the 0x1A zlib stream dies
 *       at input byte ~1117).
 *
 * The real selector of the encrypted span is still unknown, so
 * pdz_member_scope_for() returns PREFIX for every container and callers that
 * know the shape should pass an explicit scope.
 */
typedef enum PdzMemberScope {
    PDZ_MEMBER_SCOPE_AUTO = 0,   /*!< defaults to PREFIX; bookType is not a verified selector */
    PDZ_MEMBER_SCOPE_PREFIX = 1, /*!< first PDZ_MEMBER_PREFIX_SIZE bytes only */
    PDZ_MEMBER_SCOPE_WHOLE = 2,  /*!< the entire member */
} PdzMemberScope;

/* ----------------------------------------------------------- info rule ---- */

#define PDZ_INFORULE_MAGIC "BKRT"
#define PDZ_INFORULE_HEADER_SIZE 0x44

/*!
 * Header of an `inforule.dat` (BKRT).
 *
 * SOLVED 2026-10-15: bytes from 0x44 onward are a plain **zlib** stream, and
 * pdz_inforule_rule_table() inflates it into the 2024-byte rule table
 * (10x u32 tag counts + pages*8 byte page table + members*32 byte member
 * table); pdz_inforule_rule_member() / pdz_inforule_page_member() walk it.
 * See docs/authoritative/REMAINING_THREE.md.
 *
 * The header fields are offsets/sizes, NOT a page box: field_28 is the page
 * table's *size* (220*8 = 1760) and field_2c the member table's *offset*
 * (68 + 40 + 1760 = 1868). The engine renders a page to 849x1199 (A4 ratio
 * 1.412) while 1760x1868 has ratio 1.061, so the earlier page-size reading is
 * excluded; the three exact identities 40 = 10*4, 1760 = 220*8 and
 * 1868 = 68 + 40 + 1760 pin the layout down.
 */
typedef struct PdzInfoRule {
    uint32_t total_pages;  /*!< +0x14; matches meta.xml <pages> and the viewer */
    uint32_t member_count; /*!< +0x18; matches the number of *.pdg members */
    uint32_t field_1c;     /*!< +0x1C = offset of the zlib rule-table tail (68) */
    uint32_t field_20;     /*!< +0x20 = tag-count array size in bytes (40 = 10*4) */
    uint32_t field_24;     /*!< +0x24 = page-table offset in the expanded view (108 = 68+40) */
    uint32_t field_28;     /*!< +0x28 = page-table size in bytes (1760 = pages*8) */
    uint32_t field_2c;     /*!< +0x2C = member-table offset in the expanded view (1868 = 108+1760) */
    uint32_t field_30;     /*!< +0x30 = member-table size in bytes (224 = members*32) */
    size_t file_size;
    int tail_encoded;      /*!< 1 when bytes from 0x44 onward are the (zlib) table */
} PdzInfoRule;

/*! One `<first_page>_<page_count>` member name decoded into a page range. */
typedef struct PdzPageRange {
    char member[PDZ_NAME_SIZE];
    uint32_t first_page; /*!< 1-based first page covered by the member */
    uint32_t page_count; /*!< number of pages in the member */
} PdzPageRange;

/*! Structural checks against the verified layout invariants. */
typedef struct PdzValidation {
    int catalog_offset_consistent; /*!< catalog_offset + catalog_size == ssct_offset */
    int members_tile;              /*!< members cover [0x60, catalog_offset) with no gaps */
    uint32_t tiling_end;
    uint32_t tiling_gaps;
} PdzValidation;

PDG_API int pdz_abi_version(void);

/*! 1 when the buffer starts with the `%pdgz` magic. */
PDG_API int pdz_is_container(const void *data, size_t size);

PDG_API int pdz_parse(const void *data, size_t size, PdzInfo *info);

/*!
 * Decrypt the member table.
 *
 * Pass `members == NULL` (or `capacity == 0`) to query the count in `*count`
 * without filling anything. Records that fail the name/range sanity checks are
 * skipped, so `*count` may be lower than `catalog_size / 20`.
 */
PDG_API int pdz_members(const void *data,
                        size_t size,
                        PdzMember *members,
                        size_t capacity,
                        size_t *count);

PDG_API int pdz_validate(const void *data,
                         size_t size,
                         const PdzMember *members,
                         size_t count,
                         PdzValidation *out);

/*!
 * Copy one member out of the container.
 *
 * Member keys are tried in the engine's order: the certificate-derived
 * `key_string` first, then the fixed fallback key (pdz_member_key_fixed).
 * A candidate only wins when the decrypted head looks like a known member
 * plaintext (PDG `HH`, JPEG, PNG, PDF, BMP, DjVu, TIFF, XML, ZIP); otherwise
 * the requested key's result is returned untouched, so unsolvable samples
 * (04.pdz) behave exactly as before. `out->used_fixed_key` reports which key
 * won. When `key_string` is NULL the fixed key is still tried, so unencrypted
 * members keep passing through raw.
 *
 * The returned buffer must be released with pdz_buffer_free().
 */
PDG_API int pdz_member_payload(const void *data,
                               size_t size,
                               const PdzMember *member,
                               const char *key_string,
                               PdzBuffer *out);

/*! Same as pdz_member_payload() but with an explicit scope. */
PDG_API int pdz_member_payload_ex(const void *data,
                                  size_t size,
                                  const PdzMember *member,
                                  const char *key_string,
                                  int scope,
                                  PdzBuffer *out);

/*! Resolve PDZ_MEMBER_SCOPE_AUTO against this container. */
PDG_API int pdz_member_scope_for(const void *data, size_t size);

PDG_API const char *pdz_member_scope_name(int scope);

/* ------------------------------------------------------------------ 0x1A */

/*! pdg_type of a multi-page member: the payload is a zlib stream of a PDF. */
#define PDZ_PDFPDG_TYPE 0x1A
/*! Size of the fixed 0x1A member header; the zlib stream starts right after. */
#define PDZ_PDFPDG_HEADER_SIZE 0x8C
/*!
 * Number of leading member bytes that are Blowfish-ECB encrypted.
 *
 * CORRECTED 2026-10-15: the earlier "bookType == 2 -> whole member" rule was
 * wrong.  For 96304257.pdz (bookType == 2) the engine's member bytes satisfy
 * `engine[1024:] == stored[1024:]` exactly, and decoding with a 1024-byte
 * prefix reproduces the official PDFs byte-for-byte, while whole-member
 * decryption yields garbage past offset 1024.
 */
#define PDZ_PDFPDG_MEMBER_PREFIX_ENCRYPTED 1024

/*! 1 when the buffer is a pdg_type 0x1A member. */
PDG_API int pdz_pdfpdg_is_member(const void *member, size_t size);

/*!
 * Decode a pdg_type 0x1A member into PDF bytes.
 *
 * The member must already be decrypted (see
 * PDZ_PDFPDG_MEMBER_PREFIX_ENCRYPTED).  `out->data` is malloc'ed and owned by
 * the caller.
 */
PDG_API int pdz_pdfpdg_decode_member(const void *member, size_t size, PdzBuffer *out);

/* ------------------------------------------------------------- SSCT cert */

/*! ``pdgDll`` hardcoded certificate key string (12 bytes). */
#define PDZ_SSCT_HARDCODED_KEY "*s{P2a_1mP^?"

/*! ``pdgDll!DAT_101032a8``: the 94-byte key-schedule alphabet (NUL terminated). */
extern const char pdz_ssct_alphabet[95];

/*! MD5 as uppercase hex; ``out`` must hold 33 bytes. */
PDG_API void pdz_md5_hex_upper(const void *data, size_t size, char out[33]);

/*! AES-256 ECB decryption of one 16-byte block. */
PDG_API void pdz_aes256_decrypt_block(const uint8_t in[16], const uint8_t key[32],
                                      uint8_t out[16]);

/*! ``pdgDll!DAT_10103390``: the machine-key alphabet (94 chars, NUL padded). */
extern const char pdz_ssct_hdd_alphabet[95];

/*! The separator both key schedules interleave with. */
#define PDZ_SSCT_SEPARATOR "^*?&"

/*!
 * The second member-key mode: ``MD5("#$FVU&T)(*&^$#") XOR C``, i.e. the
 * pdz2pdg Blowfish key.  The engine tries this key *and* the bookkey-derived
 * one; some books only answer to this one.
 */
extern const uint8_t pdz_member_key_fixed[16];

/*! Port of ``pdgDll!FUN_10005830``; writes the variable-length uppercase hex key. */
PDG_API int pdz_ssct_hdd2key(const char *hddinfo, char *out, size_t out_size);

/*!
 * Decrypt an ``SSCT`` block with an arbitrary key string (certificate types 0
 * and 2 both funnel through here; only the key string differs).
 */
PDG_API int pdz_ssct_decode_key64(const void *blob, size_t size, const char *key64,
                                  uint8_t **out_xml, size_t *out_xml_len);

/*!
 * Recover a ``type=2`` (machine-bound) certificate by sweeping the 10,000
 * four-digit machine ids.  Writes the winning machine key to ``out_key64``.
 */
PDG_API int pdz_ssct_solve_type2(const void *blob, size_t size, char *out_key64,
                                 size_t key64_size, uint8_t **out_xml,
                                 size_t *out_xml_len);

/*! Port of ``pdgDll!FUN_10004650``; returns the 32 raw key bytes. */
PDG_API int pdz_ssct_expand_key(const char *key, const char *tail, size_t tail_len,
                                uint8_t out[32]);

/*!
 * Decode an ``SSCT...SSCE`` block into the certificate XML.
 * ``*out_xml`` is malloc'ed and owned by the caller.
 * Returns PDG_ERR_UNSUPPORTED for the ``type=2`` + ``unRegister`` combination,
 * which the official engine also refuses.
 */
PDG_API int pdz_ssct_decode(const void *blob, size_t size, uint8_t **out_xml,
                            size_t *out_xml_len);

/*! Extract ``<bookkey>`` from already-decrypted certificate XML. */
PDG_API int pdz_ssct_bookkey_from_xml(const void *xml, size_t xml_len,
                                      char *out, size_t out_size);

/*! Convenience: extract ``<bookkey>`` from an ``SSCT`` block. */
PDG_API int pdz_ssct_bookkey(const void *blob, size_t size, char *out, size_t out_size);

/*! RFC 1951 raw deflate. `*out` is malloc'ed and owned by the caller. */
PDG_API int pdz_inflate_raw(const uint8_t *in, size_t in_size, uint8_t **out, size_t *out_size);

/*! RFC 1950 zlib wrapper around pdz_inflate_raw(); verifies Adler-32. */
PDG_API int pdz_inflate_zlib(const uint8_t *in, size_t in_size, uint8_t **out, size_t *out_size);


/*! Parse the plaintext BKRT header of an inforule.dat image. */
PDG_API int pdz_inforule_parse(const void *data, size_t size, PdzInfoRule *out);

/*!
 * Decode `<first>_<count>` member names into page ranges.
 *
 * This is how the pdg_type 0x1A members of 96304257.pdz map onto the book's
 * 220 pages; the ranges are contiguous and sum to total_pages.
 */
/*! One 32-byte member entry of the decompressed rule table. */
typedef struct PdzRuleMember {
    uint32_t index;      /*!< +0x00; the tag the pages belong to */
    uint32_t first_page; /*!< +0x04; 1-based first page */
    uint32_t page_count; /*!< +0x08; number of pages */
    char name[PDZ_NAME_SIZE]; /*!< +0x0C; NUL padded member name */
} PdzRuleMember;

/*!
 * Decompressed rule table (the zlib stream at inforule +0x1C).
 *
 * Layout: 10 * u32 tag page counts, then ``total_pages`` 8-byte page entries
 * ``{u32 page_in_tag, u32 member_index_base + (member_index << 5)}``, then
 * ``member_count`` 32-byte member entries.  ``raw`` points into ``out``'s own
 * buffer and stays valid until the caller frees ``raw``.
 */
typedef struct PdzRuleTable {
    uint32_t tag_counts[10];
    uint32_t total_pages;
    uint32_t member_count;
    uint32_t page_table_offset; /*!< header +0x20; offset of the page entries */
    uint32_t member_index_base; /*!< header +0x2C */
    const uint8_t *raw;
    size_t raw_size;
} PdzRuleTable;

/*!
 * Inflate the rule table of an ``inforule.dat`` image.  ``out->raw`` is
 * malloc'ed and owned by the caller.
 */
PDG_API int pdz_inforule_rule_table(const void *data, size_t size, PdzRuleTable *out);

/*! Copy member ``index`` (0-based) out of a rule table. */
PDG_API int pdz_inforule_rule_member(const PdzRuleTable *table, uint32_t index,
                                     PdzRuleMember *out);

/*!
 * Member index for a 1-based global page -- port of pdfpdgdll!FUN_10008680.
 */
PDG_API int pdz_inforule_page_member(const PdzRuleTable *table, uint32_t page,
                                     uint32_t *out_index);

PDG_API int pdz_inforule_member_pages(const PdzMember *members,
                                      size_t count,
                                      PdzPageRange *ranges,
                                      size_t capacity,
                                      size_t *out_count);

PDG_API void pdz_buffer_free(PdzBuffer *buffer);

/*! key16 = MD5(key_string) XOR C[4]. Exposed for callers that need it directly. */
PDG_API int pdz_derive_key(const char *key_string, uint8_t key16[16]);

PDG_API const char *pdz_cipher_name(uint8_t cipher_type);

#ifdef __cplusplus
}
#endif

#endif
