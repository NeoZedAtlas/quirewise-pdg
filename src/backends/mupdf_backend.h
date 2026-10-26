#ifndef QUIREWISE_PDG_MUPDF_BACKEND_H
#define QUIREWISE_PDG_MUPDF_BACKEND_H

#include "quirewise/document.h"

#include <stddef.h>

/* Optional PDF rasterizer hook. The default build reports the external
 * decoder status and does not require MuPDF at link time. */
int qw_mupdf_render_pdf_page(const uint8_t *data,
                             size_t size,
                             size_t page_index,
                             unsigned int dpi,
                             QwPageImage *out_page,
                             void *report);

#endif
