#include "djvu_parser.h"

#include "pdg.h"
#include "quirewise/djvu.h"

#include <stddef.h>

int qw_pdg_djvu_probe(const void *data, size_t size)
{
    return qw_djvu_is_supported(data, size);
}

int qw_pdg_djvu_parse_page_count(const void *data,
                                 size_t size,
                                 size_t *out_page_count)
{
    QwDjvuStatus status;
    if (!out_page_count) return PDG_ERR_INVALID_ARGUMENT;
    *out_page_count = 0u;
    status = qw_djvu_get_page_count(data, size, out_page_count);
    switch (status) {
    case QW_DJVU_OK: return PDG_OK;
    case QW_DJVU_ERR_INVALID_ARGUMENT: return PDG_ERR_INVALID_ARGUMENT;
    case QW_DJVU_ERR_UNSUPPORTED: return PDG_ERR_UNSUPPORTED;
    case QW_DJVU_ERR_RESOURCE_LIMIT: return PDG_ERR_RESOURCE_LIMIT;
    default: return PDG_ERR_FORMAT;
    }
}
