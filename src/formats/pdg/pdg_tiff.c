#include "pdg_internal.h"

#include "../image/tiff_decode.h"

#include <string.h>

int pdg_render_tiff_payload(const uint8_t *data,
                            size_t size,
                            const PdgHeader *header,
                            PdgImage *out)
{
    QwPageImage page;
    int rc;

    if (!data || !size || !header || !out) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    memset(&page, 0, sizeof(page));
    rc = qw_tiff_decode_page(data, size, 0, &page);
    if (rc != PDG_OK) {
        return rc;
    }
    out->pixels = (uint8_t *)page.data;
    out->size = page.size;
    out->width = page.width;
    out->height = page.height;
    out->stride = page.width * 3u;
    out->format = PDG_PIXEL_RGB24;
    out->header = *header;
    return PDG_OK;
}
