#include "pdg_internal.h"
#include "../djvu/djvu_parser.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <libdjvu/ddjvuapi.h>

typedef struct PdgDjvuInput {
    const uint8_t *data;
    size_t size;
    int main_stream_supplied;
    int failed;
} PdgDjvuInput;

struct PdgDjvuReader {
    ddjvu_context_t *context;
    ddjvu_document_t *document;
    ddjvu_format_t *format;
    PdgDjvuInput input;
    size_t page_count;
};

static void djvu_feed_stream(ddjvu_document_t *document,
                             int stream_id,
                             PdgDjvuInput *input)
{
    size_t offset = 0;
    if (stream_id != 0 || input->main_stream_supplied) {
        input->failed = 1;
        ddjvu_stream_close(document, stream_id, 1);
        return;
    }
    while (offset < input->size) {
        size_t remaining = input->size - offset;
        unsigned long count = remaining > (size_t)ULONG_MAX
            ? ULONG_MAX
            : (unsigned long)remaining;
        ddjvu_stream_write(document, stream_id,
                           (const char *)(input->data + offset), count);
        offset += count;
    }
    ddjvu_stream_close(document, stream_id, 0);
    input->main_stream_supplied = 1;
}

static void djvu_process_messages(ddjvu_context_t *context,
                                  ddjvu_document_t *document,
                                  PdgDjvuInput *input,
                                  int wait_for_message)
{
    ddjvu_message_t *message;
    if (wait_for_message) {
        (void)ddjvu_message_wait(context);
    }
    while ((message = ddjvu_message_peek(context)) != NULL) {
        if (message->m_any.tag == DDJVU_NEWSTREAM) {
            djvu_feed_stream(document, message->m_newstream.streamid, input);
        } else if (message->m_any.tag == DDJVU_ERROR) {
            input->failed = 1;
        }
        ddjvu_message_pop(context);
    }
}

static int djvu_wait_job(ddjvu_context_t *context,
                         ddjvu_document_t *document,
                         PdgDjvuInput *input,
                         ddjvu_job_t *job)
{
    while (!ddjvu_job_done(job) && !input->failed) {
        djvu_process_messages(context, document, input, 1);
    }
    djvu_process_messages(context, document, input, 0);
    if (input->failed || !input->main_stream_supplied ||
        ddjvu_job_status(job) >= DDJVU_JOB_FAILED) {
        return PDG_ERR_DECODE;
    }
    return PDG_OK;
}

static void free_image_array(PdgImage *images, size_t count)
{
    if (!images) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        pdg_image_free(&images[i]);
    }
    free(images);
}

int pdg_djvu_reader_open(const uint8_t *data,
                         size_t size,
                         PdgDjvuReader **out_reader,
                         size_t *out_page_count)
{
    PdgDjvuReader *reader;
    size_t parsed_page_count;
    int rc = PDG_ERR_DECODE;

    if (!data || !size || !out_reader || !out_page_count) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    *out_reader = NULL;
    *out_page_count = 0;
    rc = qw_djvu_parse_page_count(data, size, &parsed_page_count);
    if (rc != PDG_OK) return rc;
    if (!parsed_page_count || parsed_page_count > (size_t)INT_MAX) {
        return PDG_ERR_RESOURCE_LIMIT;
    }
    reader = (PdgDjvuReader *)calloc(1, sizeof(*reader));
    if (!reader) {
        return PDG_ERR_OUT_OF_MEMORY;
    }
    reader->input.data = data;
    reader->input.size = size;

    reader->context = ddjvu_context_create("quirewise");
    if (!reader->context) {
        rc = PDG_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    reader->document = ddjvu_document_create(reader->context, NULL, 0);
    if (!reader->document) {
        goto cleanup;
    }
    rc = djvu_wait_job(reader->context, reader->document, &reader->input,
                       ddjvu_document_job(reader->document));
    if (rc != PDG_OK) {
        goto cleanup;
    }

    reader->format = ddjvu_format_create(DDJVU_FORMAT_RGB24, 0, NULL);
    if (!reader->format) {
        rc = PDG_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    ddjvu_format_set_row_order(reader->format, 1);
    ddjvu_format_set_y_direction(reader->format, 1);
    reader->page_count = parsed_page_count;
    *out_page_count = reader->page_count;
    *out_reader = reader;
    return PDG_OK;

cleanup:
    pdg_djvu_reader_close(reader);
    return rc;
}

int pdg_djvu_reader_render_page(PdgDjvuReader *reader,
                                size_t page_index,
                                const PdgHeader *header,
                                PdgImage *out)
{
    ddjvu_page_t *page;
    int width;
    int height;
    size_t stride;
    size_t pixel_size;
    uint8_t *pixels;
    ddjvu_rect_t page_rect;
    int rc;

    if (!reader || !header || !out || page_index >= reader->page_count ||
        page_index > (size_t)INT_MAX) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    page = ddjvu_page_create_by_pageno(reader->document, (int)page_index);
    if (!page) {
        return PDG_ERR_DECODE;
    }
    rc = djvu_wait_job(reader->context, reader->document, &reader->input,
                       ddjvu_page_job(page));
    if (rc != PDG_OK) {
        ddjvu_page_release(page);
        return rc;
    }
    width = ddjvu_page_get_width(page);
    height = ddjvu_page_get_height(page);
    if (width <= 0 || height <= 0 || (size_t)width > SIZE_MAX / 3u ||
        (size_t)width * 3u > SIZE_MAX / (size_t)height ||
        (size_t)width * 3u > UINT32_MAX) {
        ddjvu_page_release(page);
        return PDG_ERR_FORMAT;
    }
    stride = (size_t)width * 3u;
    pixel_size = stride * (size_t)height;
    pixels = (uint8_t *)malloc(pixel_size);
    if (!pixels) {
        ddjvu_page_release(page);
        return PDG_ERR_OUT_OF_MEMORY;
    }
    page_rect.x = 0;
    page_rect.y = 0;
    page_rect.w = (unsigned int)width;
    page_rect.h = (unsigned int)height;
    if (!ddjvu_page_render(page,
                           DDJVU_RENDER_COLOR,
                           &page_rect,
                           &page_rect,
                           reader->format,
                           (unsigned long)stride,
                           (char *)pixels)) {
        free(pixels);
        ddjvu_page_release(page);
        return PDG_ERR_DECODE;
    }
    ddjvu_page_release(page);
    out->pixels = pixels;
    out->size = pixel_size;
    out->width = (uint32_t)width;
    out->height = (uint32_t)height;
    out->stride = (uint32_t)stride;
    out->format = PDG_PIXEL_RGB24;
    out->header = *header;
    return PDG_OK;
}

void pdg_djvu_reader_close(PdgDjvuReader *reader)
{
    if (!reader) {
        return;
    }
    if (reader->format) ddjvu_format_release(reader->format);
    if (reader->document) ddjvu_document_release(reader->document);
    if (reader->context) ddjvu_context_release(reader->context);
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
    size_t image_count = 0;
    int rc;

    if (!data || !size || !header || !out_images || !out_count) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    *out_images = NULL;
    *out_count = 0;
    rc = pdg_djvu_reader_open(data, size, &reader, &document_pages);
    if (rc != PDG_OK) {
        return rc;
    }
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
            free_image_array(images, image_count);
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
    free_image_array(images, count);
    return PDG_OK;
}
