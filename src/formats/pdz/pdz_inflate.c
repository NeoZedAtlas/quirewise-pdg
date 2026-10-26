/*
 * Minimal RFC 1950 (zlib) / RFC 1951 (deflate) inflate.
 *
 * The C core deliberately has no third-party dependencies (Group4/MMR are
 * hand-rolled too), so the pdg_type 0x1A decoder carries its own inflate.
 * Canonical-Huffman decoding follows the classic counts/symbols layout.
 *
 * The whole output is kept in one growing buffer, which makes LZ77
 * back-references a plain indexed copy (no ring window bookkeeping).
 */

#include "pdz_internal.h"

#include <stdlib.h>
#include <string.h>

#define INF_MAXBITS 15
#define INF_MAXLCODES 286
#define INF_MAXDCODES 30
#define INF_MAXCODES (INF_MAXLCODES + INF_MAXDCODES)
#define INF_MAXLENS 320
#define INF_INITIAL_CAP 65536u

typedef struct {
    const uint8_t *in;
    size_t in_size;
    size_t in_pos;
    uint32_t bitbuf;
    int bitcnt;
} InfBits;

typedef struct {
    short counts[INF_MAXBITS + 1];
    short symbols[INF_MAXCODES];
} InfHuff;

typedef struct {
    uint8_t *data;
    size_t size;
    size_t cap;
} InfOut;

static const short inf_length_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
    67, 83, 99, 115, 131, 163, 195, 227, 258};
static const short inf_length_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const short inf_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const short inf_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
static const short inf_clc_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

static int inf_bits(InfBits *b, int need, uint32_t *out)
{
    while (b->bitcnt < need) {
        if (b->in_pos >= b->in_size) {
            return PDG_ERR_FORMAT;
        }
        b->bitbuf |= (uint32_t)b->in[b->in_pos++] << b->bitcnt;
        b->bitcnt += 8;
    }
    *out = b->bitbuf & ((need == 32) ? 0xFFFFFFFFu : ((1u << need) - 1u));
    b->bitbuf >>= need;
    b->bitcnt -= need;
    return PDG_OK;
}

static int inf_ensure(InfOut *o, size_t extra)
{
    size_t need = o->size + extra;
    if (need <= o->cap) {
        return PDG_OK;
    }
    while (o->cap < need) {
        o->cap = o->cap ? o->cap * 2u : INF_INITIAL_CAP;
        if (o->cap < need) {
            continue;
        }
        break;
    }
    {
        uint8_t *grown = (uint8_t *)realloc(o->data, o->cap);
        if (!grown) {
            return PDG_ERR_OUT_OF_MEMORY;
        }
        o->data = grown;
    }
    return PDG_OK;
}

/* Build a canonical Huffman decoder. Returns 0 when complete, >0 when the code
 * is incomplete, <0 on an over-subscribed or malformed code. */
static int inf_construct(InfHuff *h, const short *lengths, int n)
{
    int i, len, left;
    short offs[INF_MAXBITS + 1];

    for (i = 0; i <= INF_MAXBITS; i++) {
        h->counts[i] = 0;
    }
    for (i = 0; i < n; i++) {
        h->counts[lengths[i]]++;
    }
    if (h->counts[0] == n) {
        return 0;
    }
    left = 1;
    for (len = 1; len <= INF_MAXBITS; len++) {
        left <<= 1;
        left -= h->counts[len];
        if (left < 0) {
            return -1;
        }
    }
    offs[1] = 0;
    for (len = 1; len < INF_MAXBITS; len++) {
        offs[len + 1] = (short)(offs[len] + h->counts[len]);
    }
    for (i = 0; i < n; i++) {
        if (lengths[i]) {
            h->symbols[offs[lengths[i]]++] = (short)i;
        }
    }
    return left;
}

static int inf_decode(InfBits *b, const InfHuff *h, int *sym)
{
    int len, code = 0, first = 0, index = 0, count;

    for (len = 1; len <= INF_MAXBITS; len++) {
        uint32_t bit = 0;
        if (inf_bits(b, 1, &bit) != PDG_OK) {
            return PDG_ERR_FORMAT;
        }
        code |= (int)bit;
        count = h->counts[len];
        if (code - count < first) {
            *sym = h->symbols[index + (code - first)];
            return PDG_OK;
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return PDG_ERR_NOT_PDG;
}

static int inf_stored(InfBits *b, InfOut *o)
{
    uint32_t len, nlen;
    b->bitbuf = 0;
    b->bitcnt = 0;
    if (b->in_pos + 4 > b->in_size) {
        return PDG_ERR_FORMAT;
    }
    len = (uint32_t)b->in[b->in_pos] | ((uint32_t)b->in[b->in_pos + 1] << 8);
    nlen = (uint32_t)b->in[b->in_pos + 2] | ((uint32_t)b->in[b->in_pos + 3] << 8);
    b->in_pos += 4;
    if ((len ^ 0xFFFFu) != nlen) {
        return PDG_ERR_NOT_PDG;
    }
    if (b->in_pos + len > b->in_size) {
        return PDG_ERR_FORMAT;
    }
    if (inf_ensure(o, len) != PDG_OK) {
        return PDG_ERR_OUT_OF_MEMORY;
    }
    memcpy(o->data + o->size, b->in + b->in_pos, len);
    o->size += len;
    b->in_pos += len;
    return PDG_OK;
}

static int inf_codes(InfBits *b, InfOut *o, const InfHuff *lencode, const InfHuff *distcode)
{
    for (;;) {
        int symbol;
        if (inf_decode(b, lencode, &symbol) != PDG_OK) {
            return PDG_ERR_NOT_PDG;
        }
        if (symbol < 256) {
            if (inf_ensure(o, 1) != PDG_OK) {
                return PDG_ERR_OUT_OF_MEMORY;
            }
            o->data[o->size++] = (uint8_t)symbol;
            continue;
        }
        if (symbol == 256) {
            return PDG_OK;
        }
        symbol -= 257;
        if (symbol >= 29) {
            return PDG_ERR_NOT_PDG;
        }
        {
            uint32_t extra = 0;
            int length = inf_length_base[symbol];
            int dist_sym;
            int distance;
            size_t i;

            if (inf_length_extra[symbol] &&
                inf_bits(b, inf_length_extra[symbol], &extra) != PDG_OK) {
                return PDG_ERR_FORMAT;
            }
            length += (int)extra;
            if (inf_decode(b, distcode, &dist_sym) != PDG_OK || dist_sym >= 30) {
                return PDG_ERR_NOT_PDG;
            }
            extra = 0;
            if (inf_dist_extra[dist_sym] &&
                inf_bits(b, inf_dist_extra[dist_sym], &extra) != PDG_OK) {
                return PDG_ERR_FORMAT;
            }
            distance = inf_dist_base[dist_sym] + (int)extra;
            if ((size_t)distance > o->size) {
                return PDG_ERR_NOT_PDG; /* back-reference before the output start */
            }
            if (inf_ensure(o, (size_t)length) != PDG_OK) {
                return PDG_ERR_OUT_OF_MEMORY;
            }
            for (i = 0; i < (size_t)length; i++) {
                o->data[o->size] = o->data[o->size - (size_t)distance];
                o->size++;
            }
        }
    }
}

static int inf_fixed(InfBits *b, InfOut *o)
{
    static InfHuff lencode, distcode;
    static int built = 0;
    short lengths[288];
    int i;

    if (!built) {
        for (i = 0; i < 144; i++) {
            lengths[i] = 8;
        }
        for (; i < 256; i++) {
            lengths[i] = 9;
        }
        for (; i < 280; i++) {
            lengths[i] = 7;
        }
        for (; i < 288; i++) {
            lengths[i] = 8;
        }
        if (inf_construct(&lencode, lengths, 288) < 0) {
            return PDG_ERR_NOT_PDG;
        }
        for (i = 0; i < 30; i++) {
            lengths[i] = 5;
        }
        if (inf_construct(&distcode, lengths, 30) < 0) {
            return PDG_ERR_NOT_PDG;
        }
        built = 1;
    }
    return inf_codes(b, o, &lencode, &distcode);
}

static int inf_dynamic(InfBits *b, InfOut *o)
{
    InfHuff lencode, distcode, lenlen;
    short lengths[INF_MAXLENS];
    uint32_t hlit = 0, hdist = 0, hclen = 0;
    int i, index = 0, nlen;

    if (inf_bits(b, 5, &hlit) != PDG_OK || inf_bits(b, 5, &hdist) != PDG_OK ||
        inf_bits(b, 4, &hclen) != PDG_OK) {
        return PDG_ERR_FORMAT;
    }
    hlit += 257;
    hdist += 1;
    hclen += 4;
    if (hlit > INF_MAXLCODES || hdist > INF_MAXDCODES) {
        return PDG_ERR_NOT_PDG;
    }
    for (i = 0; i < (int)hclen; i++) {
        uint32_t v = 0;
        if (inf_bits(b, 3, &v) != PDG_OK) {
            return PDG_ERR_FORMAT;
        }
        lengths[inf_clc_order[i]] = (short)v;
    }
    for (; i < 19; i++) {
        lengths[inf_clc_order[i]] = 0;
    }
    if (inf_construct(&lenlen, lengths, 19) < 0) {
        return PDG_ERR_NOT_PDG;
    }
    nlen = (int)(hlit + hdist);
    while (index < nlen) {
        int symbol;
        if (inf_decode(b, &lenlen, &symbol) != PDG_OK) {
            return PDG_ERR_NOT_PDG;
        }
        if (symbol < 16) {
            lengths[index++] = (short)symbol;
            continue;
        }
        {
            uint32_t extra = 0;
            int repeat;
            short value = 0;
            if (symbol == 16) {
                if (index == 0) {
                    return PDG_ERR_NOT_PDG;
                }
                value = lengths[index - 1];
                if (inf_bits(b, 2, &extra) != PDG_OK) {
                    return PDG_ERR_FORMAT;
                }
                repeat = 3 + (int)extra;
            } else if (symbol == 17) {
                if (inf_bits(b, 3, &extra) != PDG_OK) {
                    return PDG_ERR_FORMAT;
                }
                repeat = 3 + (int)extra;
            } else {
                if (inf_bits(b, 7, &extra) != PDG_OK) {
                    return PDG_ERR_FORMAT;
                }
                repeat = 11 + (int)extra;
            }
            if (index + repeat > nlen) {
                return PDG_ERR_NOT_PDG;
            }
            while (repeat--) {
                lengths[index++] = value;
            }
        }
    }
    if (lengths[256] == 0) {
        return PDG_ERR_NOT_PDG;
    }
    if (inf_construct(&lencode, lengths, (int)hlit) < 0) {
        return PDG_ERR_NOT_PDG;
    }
    if (inf_construct(&distcode, lengths + hlit, (int)hdist) < 0) {
        return PDG_ERR_NOT_PDG;
    }
    return inf_codes(b, o, &lencode, &distcode);
}

int pdz_inflate_raw(const uint8_t *in, size_t in_size, uint8_t **out, size_t *out_size)
{
    InfBits b;
    InfOut o;
    int rc;

    if (!in || !out || !out_size) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    b.in = in;
    b.in_size = in_size;
    b.in_pos = 0;
    b.bitbuf = 0;
    b.bitcnt = 0;
    o.data = NULL;
    o.size = 0;
    o.cap = 0;

    for (;;) {
        uint32_t last = 0, type = 0;
        if (inf_bits(&b, 1, &last) != PDG_OK || inf_bits(&b, 2, &type) != PDG_OK) {
            rc = PDG_ERR_FORMAT;
            goto fail;
        }
        if (type == 0) {
            rc = inf_stored(&b, &o);
        } else if (type == 1) {
            rc = inf_fixed(&b, &o);
        } else if (type == 2) {
            rc = inf_dynamic(&b, &o);
        } else {
            rc = PDG_ERR_NOT_PDG;
        }
        if (rc != PDG_OK) {
            goto fail;
        }
        if (last) {
            break;
        }
    }
    if (!o.data) {
        o.data = (uint8_t *)malloc(1);
        if (!o.data) {
            return PDG_ERR_OUT_OF_MEMORY;
        }
    }
    *out = o.data;
    *out_size = o.size;
    return PDG_OK;

fail:
    free(o.data);
    return rc;
}

int pdz_inflate_zlib(const uint8_t *in, size_t in_size, uint8_t **out, size_t *out_size)
{
    int rc;

    if (!in || in_size < 6) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    if ((in[0] & 0x0F) != 8) {
        return PDG_ERR_NOT_PDG; /* CM must be deflate */
    }
    if ((((unsigned)in[0] << 8) | in[1]) % 31u != 0u) {
        return PDG_ERR_NOT_PDG; /* FCHECK */
    }
    if (in[1] & 0x20) {
        return PDG_ERR_NOT_PDG; /* preset dictionary: unsupported and never seen */
    }
    rc = pdz_inflate_raw(in + 2, in_size - 2, out, out_size);
    if (rc != PDG_OK) {
        return rc;
    }
    {
        uint32_t want = ((uint32_t)in[in_size - 4] << 24) | ((uint32_t)in[in_size - 3] << 16) |
                        ((uint32_t)in[in_size - 2] << 8) | (uint32_t)in[in_size - 1];
        uint32_t a = 1, b = 0;
        size_t i;
        for (i = 0; i < *out_size; i++) {
            a = (a + (*out)[i]) % 65521u;
            b = (b + a) % 65521u;
        }
        if (((b << 16) | a) != want) {
            free(*out);
            *out = NULL;
            *out_size = 0;
            return PDG_ERR_NOT_PDG; /* Adler-32 mismatch */
        }
    }
    return PDG_OK;
}
