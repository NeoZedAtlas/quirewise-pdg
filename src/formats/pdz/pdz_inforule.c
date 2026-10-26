/*
 * inforule.dat (BKRT) support.
 *
 * A PDZ whose members carry pdg_type 0x1A is a multi-page container: each
 * member holds a run of consecutive pages, and the per-page split plus the
 * page -> member mapping are driven by a sibling `inforule.dat` rule file that
 * the official reader parses through pdfpdgdll.dll
 * (getPdfPdgRuleFile / getPageInfo / PreparePage).
 *
 * Verified against 96304257.pdz + its inforule.dat:
 *
 *   +0x00 "BKRT"
 *   +0x04 00 01 00 00
 *   +0x08 8          +0x0C 1        +0x10 1
 *   +0x14 220        total pages    (== meta.xml <pages> == viewer pages)
 *   +0x18 7          member count   (== number of *.pdg members)
 *   +0x1C 68   +0x20 40   +0x24 108  (semantics unknown)
 *   +0x28 1760       meaning unknown
 *   +0x2C 1868       meaning unknown
 *   +0x30 224        (unknown)
 *   +0x34..0x43      zero
 *   +0x44..         encoded rule table -- the engine expands it into
 *                   tagInfoRuleCell[10] + long[10]; the encoding itself is NOT
 *                   reversed here and is reported as opaque.
 *
 * +0x28 / +0x2C are NOT a page box: the engine renders a page to an
 * 849x1199 24bpp BMP and its PDF page box is 481.92x680.4 pt (A4 ratio 1.412),
 * while 1760x1868 has ratio 1.061.
 *
 * The reader additionally exposes a per-member value through the same rule
 * table: 1868 / 1900 / 1932 were observed for the first three members, i.e.
 * the value is per member rather than global. That observation is recorded in
 * the report but is not decoded by this parser.
 */

#include "pdz_internal.h"

#include <stdlib.h>
#include <string.h>

int pdz_inforule_parse(const void *data, size_t size, PdzInfoRule *out)
{
    if (!data || !out) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    if (size < PDZ_INFORULE_HEADER_SIZE) {
        return PDG_ERR_EMPTY;
    }
    const uint8_t *bytes = (const uint8_t *)data;
    if (memcmp(bytes, PDZ_INFORULE_MAGIC, 4) != 0) {
        return PDG_ERR_FORMAT;
    }

    memset(out, 0, sizeof(*out));
    out->total_pages = qw_read_u32_le(bytes + 0x14);
    out->member_count = qw_read_u32_le(bytes + 0x18);
    out->field_1c = qw_read_u32_le(bytes + 0x1C);
    out->field_20 = qw_read_u32_le(bytes + 0x20);
    out->field_24 = qw_read_u32_le(bytes + 0x24);
    out->field_28 = qw_read_u32_le(bytes + 0x28);
    out->field_2c = qw_read_u32_le(bytes + 0x2C);
    out->field_30 = qw_read_u32_le(bytes + 0x30);
    out->file_size = size;
    out->tail_encoded = size > PDZ_INFORULE_HEADER_SIZE ? 1 : 0;
    return PDG_OK;
}

/* Parse "<first>_<count>" with an optional ".pdg" extension. Returns 1 on
 * success. Both numbers must be decimal and non-zero. */
static int parse_page_range_name(const char *name, uint32_t *first, uint32_t *count)
{
    const char *cursor = name;
    uint32_t a = 0, b = 0;

    if (*cursor < '0' || *cursor > '9') {
        return 0;
    }
    while (*cursor >= '0' && *cursor <= '9') {
        a = a * 10u + (uint32_t)(*cursor - '0');
        if (a > 1000000u) {
            return 0;
        }
        ++cursor;
    }
    if (*cursor != '_') {
        return 0;
    }
    ++cursor;
    if (*cursor < '0' || *cursor > '9') {
        return 0;
    }
    while (*cursor >= '0' && *cursor <= '9') {
        b = b * 10u + (uint32_t)(*cursor - '0');
        if (b > 1000000u) {
            return 0;
        }
        ++cursor;
    }
    if (strcmp(cursor, "") != 0 && strcmp(cursor, ".pdg") != 0) {
        return 0;
    }
    if (a == 0 || b == 0) {
        return 0;
    }
    *first = a;
    *count = b;
    return 1;
}

int pdz_inforule_member_pages(const PdzMember *members,
                              size_t count,
                              PdzPageRange *ranges,
                              size_t capacity,
                              size_t *out_count)
{
    if (!members || !out_count) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    size_t found = 0;
    uint32_t expected_first = 1;
    for (size_t i = 0; i < count; ++i) {
        /* Only regular members carry page ranges; meta.xml / cover / rule
         * side files do not. */
        size_t name_length = strlen(members[i].name);
        if (name_length < 3 || strcmp(members[i].name + name_length - 3, "pdg") != 0) {
            continue;
        }
        uint32_t first = 0, pages = 0;
        if (!parse_page_range_name(members[i].name, &first, &pages)) {
            continue;
        }
        if (ranges && found < capacity) {
            memset(&ranges[found], 0, sizeof(ranges[found]));
            memcpy(ranges[found].member, members[i].name, sizeof(ranges[found].member) - 1);
            ranges[found].first_page = first;
            ranges[found].page_count = pages;
        }
        expected_first = first + pages;
        ++found;
    }
    (void)expected_first;
    *out_count = found;
    return PDG_OK;
}


/* ------------------------------------------------------------------ rules */

int pdz_inforule_rule_table(const void *data, size_t size, PdzRuleTable *out)
{
    PdzInfoRule rule;
    uint8_t *body = NULL;
    size_t body_size = 0;
    size_t expect;
    int rc;
    uint32_t i;

    if (!data || !out) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    rc = pdz_inforule_parse(data, size, &rule);
    if (rc != PDG_OK) {
        return rc;
    }
    if (rule.file_size <= (size_t)rule.field_1c) {
        return PDG_ERR_FORMAT;
    }
    rc = pdz_inflate_zlib((const uint8_t *)data + rule.field_1c,
                          size - (size_t)rule.field_1c, &body, &body_size);
    if (rc != PDG_OK) {
        return rc;
    }
    expect = (size_t)rule.field_20 + (size_t)rule.field_28 + (size_t)rule.field_30;
    if (body_size != expect) {
        free(body);
        return PDG_ERR_FORMAT;
    }
    for (i = 0; i < 10; i++) {
        out->tag_counts[i] = qw_read_u32_le(body + i * 4);
    }
    out->total_pages = rule.total_pages;
    out->member_count = rule.member_count;
    out->page_table_offset = rule.field_20;
    out->member_index_base = rule.field_2c;
    out->raw = body;
    out->raw_size = body_size;
    return PDG_OK;
}

int pdz_inforule_rule_member(const PdzRuleTable *table, uint32_t index,
                             PdzRuleMember *out)
{
    size_t base;
    const uint8_t *entry;
    size_t n;

    if (!table || !table->raw || !out) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    if (index >= table->member_count) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    /* member table starts at +0x20 + +0x28; recover it from the first page
     * entry's offset: the caller passed the already-validated header values,
     * so recompute from raw_size - member_count*32. */
    base = table->raw_size - (size_t)table->member_count * 32u;
    entry = table->raw + base + (size_t)index * 32u;
    out->index = qw_read_u32_le(entry);
    out->first_page = qw_read_u32_le(entry + 4);
    out->page_count = qw_read_u32_le(entry + 8);
    n = 0;
    while (n + 1 < PDZ_NAME_SIZE && entry[12 + n] != 0) {
        out->name[n] = (char)entry[12 + n];
        n++;
    }
    out->name[n] = 0;
    return PDG_OK;
}

int pdz_inforule_page_member(const PdzRuleTable *table, uint32_t page,
                             uint32_t *out_index)
{
    uint32_t running = 0;
    uint32_t tag;

    if (!table || !table->raw || !out_index || page == 0) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    for (tag = 0; tag < 10; tag++) {
        uint32_t count = table->tag_counts[tag];
        if (page <= running + count) {
            size_t off = (size_t)table->page_table_offset +
                         (size_t)(running + page - 1) * 8u;
            uint32_t raw = qw_read_u32_le(table->raw + off + 4);
            if (raw < table->member_index_base) {
                return PDG_ERR_FORMAT;
            }
            *out_index = (raw - table->member_index_base) >> 5;
            return PDG_OK;
        }
        running += count;
    }
    return PDG_ERR_INVALID_ARGUMENT;
}
