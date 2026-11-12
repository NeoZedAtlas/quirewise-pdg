/* Regression coverage for the PDZ PDF-member and page-rule helpers. */
#include <pdg.h>
#include <pdz.h>
#include <pdz_pdf.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s at line %d\n", #condition, __LINE__); \
        return 1; \
    } \
} while (0)

static void set_member(PdzMember *member, const char *name, uint32_t offset,
                       uint32_t size)
{
    memset(member, 0, sizeof(*member));
    snprintf(member->name, sizeof(member->name), "%s", name);
    member->offset = offset;
    member->size = size;
}

static void put_u32_le(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

int main(void)
{
    /* FIPS-197 AES-256 ECB single-block vector. */
    static const uint8_t key[32] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
    };
    static const uint8_t cipher[16] = {
        0x8e, 0xa2, 0xb7, 0xca, 0x51, 0x67, 0x45, 0xbf,
        0xea, 0xfc, 0x49, 0x90, 0x4b, 0x49, 0x60, 0x89
    };
    static const uint8_t plain[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
    };
    static const char xml[] = "<?xml version=\"1.0\"?><pages>1</pages>";
    PdzMember members[4];
    uint8_t out[16];
    uint8_t junk[16];
    uint8_t member[0x8C + 8];
    PdzBuffer buffer;
    int rc;

    memset(out, 0, sizeof(out));
    pdz_aes256_decrypt_block(cipher, key, out);
    CHECK(memcmp(out, plain, sizeof(plain)) == 0);

    set_member(&members[0], "000001", 0x60, 4);
    set_member(&members[1], "att001", 0x64, 4);
    set_member(&members[2], "cov", 0x68, 4);
    set_member(&members[3], "meta.xml", 0, (uint32_t)(sizeof(xml) - 1u));

    CHECK(pdz_pdf_member_selected(&members[0], PDZ_PDF_PAGES_BODY) == 1);
    CHECK(pdz_pdf_member_selected(&members[1], PDZ_PDF_PAGES_BODY) == 0);
    CHECK(pdz_pdf_member_selected(&members[1], PDZ_PDF_PAGES_BODY_ATT) == 1);
    CHECK(pdz_pdf_member_selected(&members[2], PDZ_PDF_PAGES_BODY_ATT) == 0);
    CHECK(pdz_pdf_member_selected(&members[2], PDZ_PDF_PAGES_ALL) == 1);
    CHECK(pdz_pdf_member_selected(NULL, PDZ_PDF_PAGES_ALL) == 0);
    CHECK(pdz_pdf_rule_count(members, 4u, PDZ_PDF_PAGES_BODY) == 1u);
    CHECK(pdz_pdf_rule_count(members, 4u, PDZ_PDF_PAGES_BODY_ATT) == 2u);
    CHECK(pdz_pdf_rule_count(members, 4u, PDZ_PDF_PAGES_ALL) == 4u);

    /* meta.xml declares one page, which matches BODY (one six-digit member). */
    CHECK(pdz_pdf_resolve_rule(xml, sizeof(xml) - 1u, members, 4u, NULL,
                               PDZ_PDF_PAGES_AUTO) == PDZ_PDF_PAGES_BODY);
    /* Without meta.xml the resolver falls back to ALL. */
    CHECK(pdz_pdf_resolve_rule(xml, sizeof(xml) - 1u, members, 3u, NULL,
                               PDZ_PDF_PAGES_AUTO) == PDZ_PDF_PAGES_ALL);
    CHECK(pdz_pdf_resolve_rule(xml, sizeof(xml) - 1u, members, 4u, NULL,
                               PDZ_PDF_PAGES_BODY_ATT) ==
          PDZ_PDF_PAGES_BODY_ATT);
    CHECK(pdz_pdf_resolve_rule(xml, sizeof(xml) - 1u, members, 4u, NULL, 9) ==
          PDG_ERR_INVALID_ARGUMENT);

    memset(junk, 0, sizeof(junk));
    memset(&buffer, 0, sizeof(buffer));
    CHECK(pdz_pdfpdg_decode_member(junk, sizeof(junk), &buffer) != PDG_OK);
    CHECK(buffer.data == NULL);

    /* A syntactically valid 0x1A header with a non-zlib payload must fail. */
    memset(member, 0, sizeof(member));
    member[0] = 'H';
    member[1] = 'H';
    member[2] = 1;
    member[0x0F] = 0x1A;
    put_u32_le(member + 0x18, 0x8C);
    put_u32_le(member + 0x1C, 8u);
    CHECK(pdz_pdfpdg_is_member(member, sizeof(member)) == 1);
    memset(&buffer, 0, sizeof(buffer));
    rc = pdz_pdfpdg_decode_member(member, sizeof(member), &buffer);
    CHECK(rc != PDG_OK);
    CHECK(buffer.data == NULL);
    pdz_buffer_free(&buffer);

    puts("PDZ PDF member helpers OK");
    return 0;
}
