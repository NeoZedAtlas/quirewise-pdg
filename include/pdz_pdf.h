#ifndef PDZ_PDF_H
#define PDZ_PDF_H

/*
 * PDF output for the PDZ layer: a self-contained writer, image embedder and
 * classic-xref merger. No third-party dependency, consistent with the rest of
 * C_Performance.
 *
 * The standalone writer currently provides the image-page route. Native PDF
 * member merging is exposed as a separate API and reports unsupported until a
 * validated merger is available.
 *
 *   Route 1 ("native PDF members")
 *       Some containers store their pages as pdg_type 0x1A members whose payload
 *       is a zlib stream holding a complete publisher PDF. Those member PDFs are
 *       the genuine vector original, so the only work left is merging them into
 *       one file (pdz_pdf_from_pdz). 96304257.pdz / 01.pdz / 03.pdz.
 *
 *   Route 2 ("image pages")
 *       Everything else stores one page per member. Each page is recovered as a
 *       complete image bitstream and embedded verbatim: JPEG through
 *       /DCTDecode, CCITT Group4/MMR through /CCITTFaxDecode. Nothing is
 *       re-encoded, so the pages are lossless with respect to the source.
 *       02.pdz / 05.pdz / 06.pdz / 15049151.pdz / 666.pdz.
 *
 * Route 2 page selection is convention-based (SSReader member naming), NOT
 * oracle-verified; see PDZ_PDF_PAGES_AUTO and docs/authoritative/PDZ_TO_PDF.md.
 */

#include "pdz.h"
#include "quirewise/document.h"

#ifdef __cplusplus
extern "C" {
#endif

/*! How a PDF page is sized when the page is an image (route 2). */
typedef enum PdzPdfPageMode {
    PDZ_PDF_PAGE_A4 = 0,   /*!< A4 portrait, image fitted and centred */
    PDZ_PDF_PAGE_IMAGE = 1 /*!< page box == image pixel size at PDZ_PDF_DPI */
} PdzPdfPageMode;

#define PDZ_PDF_DPI 300.0

/*!
 * Which members become pages (route 2).
 *
 * SSReader names book members by role: ``fowNNN`` foreword, ``!NNNNN`` table of
 * contents, ``NNNNNN`` body page, ``attNNN`` attachment, plus ``cov``/``bok``/
 * ``leg`` front matter. Only BODY+BODY_ATT reproduce the container's own
 * ``meta.xml`` page count, and only for some books.
 */
typedef enum PdzPdfPageRule {
    PDZ_PDF_PAGES_AUTO = 0,  /*!< pick the rule whose count matches meta.xml <pages> */
    PDZ_PDF_PAGES_BODY = 1,  /*!< NNNNNN.pdg only */
    PDZ_PDF_PAGES_BODY_ATT = 2, /*!< NNNNNN.pdg + attNNN.pdg */
    PDZ_PDF_PAGES_ALL = 3    /*!< every *.pdg member; recognized image pages use section order */
} PdzPdfPageRule;

#define PDZ_PDF_ROUTE_NONE 0
#define PDZ_PDF_ROUTE_MEMBERS 1 /*!< merged native 0x1A member PDFs */
#define PDZ_PDF_ROUTE_IMAGES 2  /*!< pages rebuilt from image members */

/*! What pdz_pdf_from_pdz() actually did, for diagnostics and acceptance. */
typedef struct PdzPdfReport {
    int route;                /*!< PDZ_PDF_ROUTE_* */
    int page_rule;            /*!< the rule used (resolved, never AUTO) */
    uint32_t declared_pages;  /*!< meta.xml <pages>; 0 when absent */
    size_t members_total;     /*!< members in the catalog */
    size_t members_used;      /*!< members that became pages */
    size_t pdf_members;       /*!< 0x1A members merged (route 1) */
    size_t pages_written;     /*!< pages in the produced PDF */
    size_t images_jpeg;       /*!< pages embedded via /DCTDecode */
    size_t images_ccitt;      /*!< pages embedded via /CCITTFaxDecode */
    size_t images_raw;        /*!< pages embedded uncompressed */
    size_t skipped;           /*!< members that could not become a page */
    size_t rule_counts[4];    /*!< member count each rule would select */
    char note[192];           /*!< human-readable caveat, may be empty */
} PdzPdfReport;

/*!
 * Convert an image-page ``%pdgz`` container into a single book PDF.
 *
 * Containers with native PDF members (``pdg_type 0x1A``) report
 * PDG_ERR_UNSUPPORTED because native member merging is not silently treated as
 * an image conversion. `key_string` may be NULL: member keys are then resolved
 * the same way as pdz_member_payload() (bookkey -> fixed fallback -> stored as-is).
 *
 * The caller owns ``*out_pdf`` and releases it with free().
 */
PDG_API int pdz_pdf_from_pdz(const void *data,
                             size_t size,
                             const char *key_string,
                             int page_rule,
                             int page_mode,
                             uint8_t **out_pdf,
                             size_t *out_size,
                             PdzPdfReport *report);

/*!
 * Merge already-decoded PDF blobs into one document. The current standalone
 * package reports PDG_ERR_UNSUPPORTED until the native merger is validated.
 *
 * The arguments are retained for ABI compatibility with existing callers.
 */
PDG_API int pdz_pdf_merge(const uint8_t *const *pdfs,
                          const size_t *sizes,
                          size_t count,
                          uint8_t **out_pdf,
                          size_t *out_size,
                          size_t *out_pages);

/*!
 * Build a PDF from standalone page images.
 *
 * Every entry is a *complete* bitstream that PDF can carry as an image XObject,
 * so nothing is re-encoded and the pages stay lossless with respect to the
 * source. Used by route 2 and by the ``--pdg DIR`` mode, where each input is a
 * single-page PDG file.
 */
/* Compatibility names retained for existing PDZ/PDG consumers. */
typedef QwPageImageKind PdzPdfImageKind;
typedef QwPageImage PdzPdfImage;
#define PDZ_PDF_IMG_JPEG QW_PAGE_IMAGE_JPEG
#define PDZ_PDF_IMG_CCITT QW_PAGE_IMAGE_CCITT
#define PDZ_PDF_IMG_GRAY8 QW_PAGE_IMAGE_GRAY8
#define PDZ_PDF_IMG_RGB24 QW_PAGE_IMAGE_RGB24
#define PDZ_PDF_IMG_JP2 QW_PAGE_IMAGE_JP2
#define PDZ_PDF_IMG_JP2_ALPHA QW_PAGE_IMAGE_JP2_ALPHA
#define PDZ_PDF_IMG_JP2_ALPHA_PREMULTIPLIED QW_PAGE_IMAGE_JP2_ALPHA_PREMULTIPLIED

PDG_API int pdz_pdf_from_images(const PdzPdfImage *images,
                                size_t count,
                                int page_mode,
                                uint8_t **out_pdf,
                                size_t *out_size,
                                size_t *out_pages);

/*! Release a PDF buffer returned by any pdz_pdf_* producer. */
PDG_API void pdz_pdf_free(void *pdf);

/*! ``<pages>`` from a container's meta.xml, or 0 when it cannot be read. */
PDG_API uint32_t pdz_pdf_meta_pages(const void *xml, size_t size);

/*!
 * Build a PDF from standalone single-page PDG files (or direct image files),
 * in the given order. This is the C equivalent of what the Qt GUI does by
 * rasterising, except the pages are carried verbatim instead of re-encoded.
 */
PDG_API int pdz_pdf_from_pdg_files(const uint8_t *const *blobs,
                                   const size_t *sizes,
                                   size_t count,
                                   int page_mode,
                                   uint8_t **out_pdf,
                                   size_t *out_size,
                                   size_t *out_pages,
                                   size_t *out_skipped);

/*! Number of page members the given rule selects (for --pages diagnostics). */
PDG_API size_t pdz_pdf_rule_count(const PdzMember *members, size_t count, int rule);

PDG_API const char *pdz_pdf_page_rule_name(int rule);
PDG_API const char *pdz_pdf_route_name(int route);

#ifdef __cplusplus
}
#endif

#endif
