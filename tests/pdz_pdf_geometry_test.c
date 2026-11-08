/*
 * Regression: the page box and the content stream must use the same units.
 *
 * The writer sized the page in points (pixels * 72 / PDZ_PDF_DPI) but emitted
 * the image matrix in pixels, so every image was scaled up by DPI/72 (~4.17x)
 * and overflowed its page. In A4 mode the documented "fitted and centred"
 * behaviour did not exist at all.
 */

#include <pdz_pdf.h>

#include <quirewise/document.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int contains(const uint8_t *data, size_t size, const char *text)
{
    size_t length = strlen(text);
    size_t index;
    if (!length || size < length) return 0;
    for (index = 0u; index + length <= size; ++index) {
        if (memcmp(data + index, text, length) == 0) return 1;
    }
    return 0;
}

static int check_page(const char *label, const uint8_t *pdf, size_t size,
                      size_t pages, const char *media_box,
                      const char *content_matrix)
{
    if (size < 8u || memcmp(pdf, "%PDF-", 5u) != 0 || pages != 1u) {
        fprintf(stderr, "%s: bad header or page count\n", label);
        return 0;
    }
    if (!contains(pdf, size, media_box)) {
        fprintf(stderr, "%s: missing media box %s\n", label, media_box);
        return 0;
    }
    if (!contains(pdf, size, content_matrix)) {
        fprintf(stderr, "%s: missing content matrix %s\n", label,
                content_matrix);
        return 0;
    }
    return 1;
}

int main(void)
{
    uint8_t pixels[600u * 300u * 3u];
    PdzPdfImage image;
    uint8_t *pdf = NULL;
    size_t pdf_size = 0u;
    size_t pages = 0u;
    int rc;
    int ok = 1;

    memset(pixels, 0x80, sizeof(pixels));
    memset(&image, 0, sizeof(image));
    image.data = pixels;
    image.kind = QW_PAGE_IMAGE_RGB24;

    /* IMAGE mode: 300 px at 300 DPI is exactly 72 pt. */
    image.width = 300u;
    image.height = 300u;
    image.size = 300u * 300u * 3u;
    pdf = NULL;
    pdf_size = 0u;
    pages = 0u;
    rc = pdz_pdf_from_images(&image, 1u, PDZ_PDF_PAGE_IMAGE, &pdf, &pdf_size,
                             &pages);
    if (rc != PDG_OK) {
        fprintf(stderr, "image mode: rc=%d\n", rc);
        return 1;
    }
    ok = check_page("image mode", pdf, pdf_size, pages,
                    "/MediaBox [0 0 72.000 72.000]",
                    "q 72.000 0 0 72.000 0.000 0.000 cm /Im0 Do Q");
    free(pdf);
    if (!ok) return 2;

    /* A4 mode: 600x300 px is 144x72 pt; fit by width, centre vertically. */
    image.width = 600u;
    image.height = 300u;
    image.size = 600u * 300u * 3u;
    pdf = NULL;
    pdf_size = 0u;
    pages = 0u;
    rc = pdz_pdf_from_images(&image, 1u, PDZ_PDF_PAGE_A4, &pdf, &pdf_size,
                             &pages);
    if (rc != PDG_OK) {
        fprintf(stderr, "a4 mode: rc=%d\n", rc);
        return 3;
    }
    ok = check_page("a4 mode", pdf, pdf_size, pages,
                    "/MediaBox [0 0 595.276 841.890]",
                    "q 595.276 0 0 297.638 0.000 272.126 cm /Im0 Do Q");
    free(pdf);
    if (!ok) return 4;
    return 0;
}
