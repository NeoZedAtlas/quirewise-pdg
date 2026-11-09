# Quirewise PDG/PDZ

Standalone PDG decoder, renderer, and PDZ container library.

The package exports Quirewise::PDG and the public headers pdg.h, pdz.h,
and pdz_pdf.h. It can be consumed without the page library; image rendering
uses the separately installable QuirewiseImage package.

## Capabilities

- Decodes SSReader PDG pages: private Group 4 bitmaps, JPEG, PNG, DjVu and the
  PDZ container variants. Encrypted headers are unwrapped internally.
- `pdg_decode_memory` returns the page in its native encoding when it has one
  and a BMP otherwise; `pdg_render_to_ccitt` re-encodes bitonal pages as
  standard ITU-T T.6 (`PDG_OUTPUT_CCITT`) so a writer can embed them with
  `/CCITTFaxDecode`. A 2402x3307 page drops from roughly 8 MB of samples to
  about 200 kB.
- `pdz_pdf_from_images` and the streaming writer (`pdz_pdf_writer_create`,
  `pdz_pdf_writer_add_page`, `pdz_pdf_writer_finish`, `pdz_pdf_writer_destroy`)
  assemble PDFs. The streaming form hands each object to a caller sink and
  writes an incremental xref, so peak memory is one page instead of one book.
  JPEG pages pass through untouched (`/DCTDecode`); the colour space
  (`/DeviceGray`, `/DeviceRGB`, `/DeviceCMYK`) is taken from the JPEG frame.
- File entry points take UTF-8 paths on every platform.

## Limits

- Pages that carry a native encoding are embedded verbatim; `pdg_render_to_ccitt`
  reports `PDG_ERR_UNSUPPORTED` for them by design.
- The T.6 encoder emits horizontal-mode pairs only: valid and lossless, about a
  third larger than an optimal encoder.

## Tests

`ctest` runs the decoder smoke, the fuzz smoke, the parallelism stress test and
the PDF writer suites, including the streaming writer golden comparison and the
Group 4 encoder goldens (cross-checked with MuPDF).