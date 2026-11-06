#include "pdg_internal.h"
#include "../djvu/djvu_parser.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* DjVu support in Quirewise is being replaced by an internal decoder.
 *
 * The structure parser (src/formats/djvu/djvu_parser.c) already validates
 * FORM/DJVU/DJVM containers and derives the page count. Pixel rendering
 * (BZZ, JB2, IW44) is not implemented yet, so every render request returns a
 * stable PDG_ERR_UNSUPPORTED without publishing partial output. No third-party
 * DjVu library is linked, included, or discovered by the build. */

struct PdgDjvuReader {
    const uint8_t *data;
    size_t size;
    size_t page_count;
};

int pdg_djvu_reader_open(const uint8_t *data,
                         size_t size,
                         PdgDjvuReader **out_reader,
                         size_t *out_page_count)
{
    PdgDjvuReader *reader;
    size_t parsed_page_count = 0;
    int rc;

    if (!data || !size || !out_reader || !out_page_count) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    *out_reader = NULL;
    *out_page_count = 0;
    rc = qw_pdg_djvu_parse_page_count(data, size, &parsed_page_count);
    if (rc != PDG_OK) return rc;
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
    if (!reader || !header || !out || page_index >= reader->page_count) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    /* Container and page count are validated, but the internal BZZ/JB2/IW44
     * decoder does not exist yet. Report a deterministic unsupported status so
     * callers never receive partial or mis-decoded pixels. */
    return PDG_ERR_UNSUPPORTED;
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
    rc = pdg_djvu_reader_render_page(reader, 0u, header, &images[0]);
    if (rc != PDG_OK) {
        free(images);
        pdg_djvu_reader_close(reader);
        return rc;
    }
    for (size_t index = 1; index < image_count; ++index) {
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
