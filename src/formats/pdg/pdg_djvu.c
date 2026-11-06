#include "pdg_internal.h"
#include "../djvu/djvu_parser.h"
#include "quirewise/djvu.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* DjVu decoding is delegated to the Page package's internal decoder
 * (vendored MIT djvudec). The PDG structure probe and page count use the same
 * entry points, so structure validation and rendering share one bounds check. */

struct PdgDjvuReader {
    const uint8_t *data;
    size_t size;
    size_t page_count;
};

static int pdg_map_djvu_status(QwDjvuStatus status)
{
    switch (status) {
    case QW_DJVU_OK: return PDG_OK;
    case QW_DJVU_ERR_INVALID_ARGUMENT: return PDG_ERR_INVALID_ARGUMENT;
    case QW_DJVU_ERR_UNSUPPORTED: return PDG_ERR_UNSUPPORTED;
    case QW_DJVU_ERR_RESOURCE_LIMIT: return PDG_ERR_RESOURCE_LIMIT;
    default: return PDG_ERR_FORMAT;
    }
}

int pdg_djvu_reader_open(const uint8_t *data,
                         size_t size,
                         PdgDjvuReader **out_reader,
                         size_t *out_page_count)
{
    PdgDjvuReader *reader;
    size_t parsed_page_count = 0;
    QwDjvuStatus status;

    if (!data || !size || !out_reader || !out_page_count) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    *out_reader = NULL;
    *out_page_count = 0;
    status = qw_djvu_get_page_count(data, size, &parsed_page_count);
    if (status != QW_DJVU_OK) return pdg_map_djvu_status(status);
    if (!parsed_page_count || parsed_page_count > (size_t)INT_MAX) {
        return PDG_ERR_RESOURCE_LIMIT;
    }
    reader = (PdgDjvuReader *)calloc(1, sizeof(*reader));
    if (!reader) return PDG_ERR_OUT_OF_MEMORY;
    reader->data = data;
    reader->size = size;
    reader->page_count = parsed_page_count;
    *out_page_count = parsed_page_count;
    *out_reader = reader;
    return PDG_OK;
}

int pdg_djvu_reader_render_page(PdgDjvuReader *reader,
                                size_t page_index,
                                const PdgHeader *header,
                                PdgImage *out)
{
    QwDjvuImage image;
    QwDjvuStatus status;

    if (!reader || !header || !out || page_index >= reader->page_count) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    memset(&image, 0, sizeof(image));
    status = qw_djvu_render_page(reader->data, reader->size, page_index, 1u,
                                 &image);
    if (status != QW_DJVU_OK) return pdg_map_djvu_status(status);
    out->pixels = image.pixels;
    out->size = image.size;
    out->width = (uint32_t)image.width;
    out->height = (uint32_t)image.height;
    out->stride = (uint32_t)image.stride;
    out->format = image.format == QW_DJVU_PIXEL_RGB24 ? PDG_PIXEL_RGB24
                                                      : PDG_PIXEL_GRAY8;
    out->header = *header;
    image.pixels = NULL;
    qw_djvu_image_free(&image);
    return PDG_OK;
}

void pdg_djvu_reader_close(PdgDjvuReader *reader)
{
    free(reader);
}

static int render_djvu_pages(const uint8_t *data,
                             size_t size,
                             const PdgHeader *header,
                             size_t page_limit,
                             PdgImage **out_images,
                             size_t *out_count)
{
    PdgDjvuReader *reader = NULL;
    PdgImage *images = NULL;
    size_t document_pages = 0;
    size_t image_count;
    int rc;

    if (!data || !size || !header || !out_images || !out_count) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    *out_images = NULL;
    *out_count = 0;
    rc = pdg_djvu_reader_open(data, size, &reader, &document_pages);
    if (rc != PDG_OK) return rc;
    image_count = page_limit && page_limit < document_pages
        ? page_limit
        : document_pages;
    if (image_count > SIZE_MAX / sizeof(*images)) {
        pdg_djvu_reader_close(reader);
        return PDG_ERR_FORMAT;
    }
    images = (PdgImage *)calloc(image_count, sizeof(*images));
    if (!images) {
        pdg_djvu_reader_close(reader);
        return PDG_ERR_OUT_OF_MEMORY;
    }
    for (size_t index = 0; index < image_count; ++index) {
        rc = pdg_djvu_reader_render_page(reader, index, header, &images[index]);
        if (rc != PDG_OK) {
            for (size_t done = 0; done < index; ++done) {
                pdg_image_free(&images[done]);
            }
            free(images);
            pdg_djvu_reader_close(reader);
            return rc;
        }
    }
    pdg_djvu_reader_close(reader);
    *out_images = images;
    *out_count = image_count;
    return PDG_OK;
}

int pdg_render_djvu_pages(const uint8_t *data,
                          size_t size,
                          const PdgHeader *header,
                          PdgImage **out_images,
                          size_t *out_count)
{
    return render_djvu_pages(data, size, header, 0, out_images, out_count);
}

int pdg_render_djvu_payload(const uint8_t *data,
                            size_t size,
                            const PdgHeader *header,
                            PdgImage *out)
{
    PdgImage *images = NULL;
    size_t count = 0;
    int rc;
    if (!out) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    rc = render_djvu_pages(data, size, header, 1, &images, &count);
    if (rc != PDG_OK) {
        return rc;
    }
    if (!count) {
        free(images);
        return PDG_ERR_DECODE;
    }
    *out = images[0];
    memset(&images[0], 0, sizeof(images[0]));
    free(images);
    return PDG_OK;
}
