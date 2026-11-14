# quirewise-pdg usage guide

This guide covers `Quirewise::PDG`. The public API lives in `pdg.h`, `pdz.h` and
`pdz_pdf.h` at the include root.

## 1. Build and integrate

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/prefix
```

```cmake
find_package(QuirewisePDG CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE Quirewise::PDG)
```

Headers are installed at the include root (`#include <pdg.h>`). Only
`pdg_decode_file`/`pdg_decrypt_file` read files (UTF-8 paths on every
platform); all other entry points take memory buffers.

## 2. Core model: decode, render, write

Three layers, often used together:

- **Decode** (`pdg_decode_memory`) unwraps the header and returns the native
  encoding (`PdgBuffer.kind`: BMP, JPEG, PNG, DjVu, TIFF, PDF, JP2, CCITT, raw).
- **Render** (`pdg_render_memory`) produces `PdgImage` pixels (Gray8/RGB24) for
  payloads this package can rasterise locally.
- **Write** (`pdz_pdf_*`) embeds pages into a new PDF.

`PdgHeader` is filled for PDG files; for a bare image, the header is zeroed and
`kind` identifies the image.

Ownership: `pdg_buffer_free` releases `PdgBuffer.data`, `pdg_image_free`
releases `PdgImage.pixels`, `pdz_buffer_free` releases `PdzBuffer.data`, and
`pdz_pdf_free` releases a PDF; input buffers are borrowed.

## 3. Decode a PDG page

```c
#include <pdg.h>
#include <stdio.h>
PdgBuffer buffer = {0};
int rc = pdg_decode_file("page-0001.pdg", &buffer);
if (rc != PDG_OK) {
    fprintf(stderr, "%s\n", pdg_error_string(rc));
    return 1;
}
printf("type=0x%02x %ux%u kind=%s bytes=%zu\n", buffer.header.type,
       buffer.header.width, buffer.header.height,
       pdg_output_kind_name(buffer.kind), buffer.size);
pdg_buffer_free(&buffer);   /* buffer.kind == PDG_OUTPUT_BMP -> a full BMP */
```

Use `pdg_decrypt_memory` to unwrap only, or `pdg_decode_memory` for the full
decode. Both accept a bare image and copy it through with its detected kind.
A `PdgContext` is optional: it caches Group 4 reference rows. Create it with
`pdg_context_create`, pass it to the `*_ctx` variants, and free it with
`pdg_context_free` (`NULL` is always accepted).

## 4. Render a page

```c
PdgImage image = {0};

rc = pdg_render_memory(bytes, size, &image);
if (rc == PDG_ERR_EXTERNAL_DECODER_REQUIRED) {
    /* PDF or JP2 payload: use a PDF/JP2 renderer instead */
} else if (rc != PDG_OK) {
    fprintf(stderr, "%s\n", pdg_error_string(rc));
} else {
    /* image.pixels / width / height / stride / format */
    pdg_image_free(&image);
}
```

Group 4, BMP, JPEG, PNG, DjVu and TIFF payloads rasterise locally; PDF and JP2
return `PDG_ERR_EXTERNAL_DECODER_REQUIRED`.

## 5. Batch decode or render

Batch calls write one `status` per item and return the first error (or
`PDG_OK`). `jobs` is clamped to `1..PDG_MAX_PARALLEL_JOBS` (8), one context per
worker.

```c
PdgRenderItem *items = calloc(count, sizeof(*items));
if (!items) return 1;
for (size_t i = 0; i < count; ++i) {
    items[i].data = pages[i];
    items[i].size = page_sizes[i];
}
(void)pdg_render_memory_batch(items, count, 4);
for (size_t i = 0; i < count; ++i)
    if (items[i].status == PDG_OK) pdg_image_free(&items[i].image);
free(items);
```

Use `PdgBatchItem` with the decode/decrypt batch variants; `count == 0` returns
`PDG_OK`.

## 6. PDZ containers

```c
/* %pdgz: 0x60-byte header, back-to-back members, encrypted catalog, SSCT */
#include <pdz.h>
PdzInfo info;
PdzMember *members = NULL;
size_t count = 0;

if (pdz_parse(data, size, &info) != PDG_OK) return 1;
if (pdz_members(data, size, NULL, 0, &count) != PDG_OK || !count) return 1;
members = calloc(count, sizeof(*members));
if (!members) return 1;
if (pdz_members(data, size, members, count, &count) != PDG_OK) {
    free(members);
    return 1;
}
for (size_t i = 0; i < count; ++i) {
    PdzBuffer payload = {0};
    if (pdz_member_payload(data, size, &members[i], bookkey, &payload) != PDG_OK)
        continue;
    /* payload.data / size; flags: prefix_decrypted, used_fixed_key, ... */
    pdz_buffer_free(&payload);
}
free(members);
```

- `bookkey` may be `NULL`: the fixed fallback key is still tried and plaintext
  members return unchanged. `PDZ_MEMBER_SCOPE_AUTO` resolves to `PREFIX`
  (first 1024 bytes).
- `pdz_validate` checks that members tile `[0x60, catalog_offset)` and that the
  catalog ends at an `SSCT` marker.

```c
char bookkey[256];
if (info.ssct_offset && pdz_ssct_bookkey((const uint8_t *)data + info.ssct_offset,
    info.file_size - info.ssct_offset, bookkey, sizeof(bookkey)) == PDG_OK) {
    /* pass bookkey to pdz_member_payload */
}
```

`pdz_ssct_decode` returns the certificate XML, `pdz_ssct_bookkey_from_xml`
extracts `<bookkey>`, and `pdz_ssct_solve_type2` sweeps machine ids. A
`pdg_type 0x1A` member holds a zlib stream with a publisher PDF:

```c
if (pdz_pdfpdg_is_member(payload.data, payload.size)) {
    PdzBuffer pdf_member = {0};
    if (pdz_pdfpdg_decode_member(payload.data, payload.size, &pdf_member) ==
        PDG_OK)
        pdz_buffer_free(&pdf_member);   /* complete PDF; caller merges */
}
```

For containers split by a sibling `inforule.dat` (BKRT), use
`pdz_inforule_parse` + `pdz_inforule_rule_table` and map pages with
`pdz_inforule_page_member`; free `table.raw` with `free`.

## 7. Build a PDF

Encoded JPEG, T.6 and JP2 pages use `/DCTDecode`, `/CCITTFaxDecode` and
`/JPXDecode`; Gray8/RGB24 samples are stored uncompressed. `PDZ_PDF_PAGE_A4`
fits and centres; `PDZ_PDF_PAGE_IMAGE` uses the image size at `PDZ_PDF_DPI`.

```c
PdzPdfImage image = {0};
uint8_t *pdf = NULL;
size_t pdf_size = 0, pages = 0;

image.kind = PDZ_PDF_IMG_GRAY8;   /* or _RGB24, _JPEG, _CCITT, _JP2 */
image.data = gray_pixels;
image.size = gray_size;
image.width = width; image.height = height;
if (pdz_pdf_from_images(&image, 1, PDZ_PDF_PAGE_A4, &pdf, &pdf_size, &pages) == PDG_OK) {
    pdz_pdf_free(pdf);            /* pdf[0..pdf_size) holds `pages` pages */
}
```

Converting a container:

```c
PdzPdfReport report;
uint8_t *pdf = NULL;
size_t pdf_size = 0;
if (pdz_pdf_from_pdz(data, size, bookkey, PDZ_PDF_PAGES_AUTO, PDZ_PDF_PAGE_A4,
                     &pdf, &pdf_size, &report) == PDG_ERR_UNSUPPORTED &&
    report.route == PDZ_PDF_ROUTE_MEMBERS) {
    /* native 0x1A members: decode report.pdf_members and merge */
} else if (pdf) {
    pdz_pdf_free(pdf);
}
```

`pdz_pdf_merge` always returns `PDG_ERR_UNSUPPORTED`; merging belongs to
`Quirewise::Document`. `pdz_pdf_resolve_rule` resolves `PAGES_AUTO` against
`meta.xml`, and `pdz_pdf_rule_count`/`pdz_pdf_member_selected` expose the
selection predicates. The streaming writer keeps peak memory at one page:

```c
static int sink(void *ctx, const void *data, size_t size)
{ return fwrite(data, 1, size, (FILE *)ctx) == size ? 0 : -1; }

PdzPdfWriter *writer = NULL;
if (pdz_pdf_writer_create(PDZ_PDF_PAGE_A4, sink, file, &writer) == PDG_OK) {
    pdz_pdf_writer_add_page(writer, &image);
    pdz_pdf_writer_finish(writer, &pages);   /* writes xref + trailer */
    pdz_pdf_writer_destroy(writer);
}
```

A non-zero sink return aborts with `PDG_ERR_IO`; destroying a writer without a
successful finish leaves a partial stream behind.

## 8. Re-encode a bitonal page as T.6

```c
PdgBuffer ccitt = {0};
if (pdg_render_to_ccitt(bytes, size, &ccitt) == PDG_OK) {
    /* embed with /CCITTFaxDecode /BlackIs1 true; columns = header.width */
    pdg_buffer_free(&ccitt);
}
```

JPEG, PNG and JP2 pages report `PDG_ERR_UNSUPPORTED` because they already carry
an encoding; the T.6 stream is valid and about a third larger than optimal.

## 9. Errors, memory and threading

| Code | Meaning |
|---|---|
| `PDG_ERR_FORMAT` / `PDG_ERR_NOT_PDG` / `PDG_ERR_NOT_PDZ` | wrong signature or invalid structure |
| `PDG_ERR_UNSUPPORTED` | known container, unsupported feature or variant |
| `PDG_ERR_EXTERNAL_DECODER_REQUIRED` | payload is PDF or JP2 |
| `PDG_ERR_RESOURCE_LIMIT` | a size or pixel limit was exceeded |

`PDG_ERR_DECODE`, `PDG_ERR_IO`, `PDG_ERR_EMPTY` and `PDG_ERR_OUT_OF_MEMORY`
cover the remaining paths.

- Input buffers are borrowed for the call; PDZ member extraction copies.
- `PdgContext` is not thread-safe; use one per thread. Batch items always get
  a `status`, and PDF/JP2 bytes belong to the caller.

## 10. Related packages

- `Quirewise::Image` supplies the raster decoders for JPEG/PNG/BMP/TIFF.
- `Quirewise::Page` supplies DjVu decoding and page-container APIs.
- `Quirewise::Document` renders PDF/JP2 and merges native PDF members.
- `Quirewise::Crypto` provides the PDZ MD5/AES primitives.
