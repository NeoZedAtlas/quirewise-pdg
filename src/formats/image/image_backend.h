#ifndef QUIREWISE_PDG_IMAGE_BACKEND_H
#define QUIREWISE_PDG_IMAGE_BACKEND_H

#include "quirewise/document.h"

#include <stddef.h>
#include <stdint.h>

/* These functions are supplied by the independently installable image
 * package.  They are intentionally declared here instead of importing the
 * page library's private headers. */
int qw_bmp_decode_page(const void *data, size_t size, QwPageImage *out_page);
int qw_png_probe(const void *data, size_t size);
int qw_png_decode_page(const void *data, size_t size, QwPageImage *out_page);
int qw_jpeg_parse(const void *data, size_t size,
                  uint32_t *out_width, uint32_t *out_height);
void qw_image_release_page(QwPageImage *page);
int qw_tiff_decode_page(const uint8_t *data, size_t size,
                        size_t page_index, QwPageImage *out_page);

static inline int qw_pdg_image_dimensions_within_limits(uint32_t width,
                                                         uint32_t height)
{
    return width != 0u && height != 0u && width <= 32768u && height <= 32768u &&
           (size_t)width <= (size_t)20000000u / (size_t)height;
}

#endif
