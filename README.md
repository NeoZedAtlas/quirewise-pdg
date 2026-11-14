# quirewise-pdg

SSReader PDG page decoding and rasterisation, PDZ container access, and a
self-contained PDZ-to-PDF writer.

`Quirewise::PDG` is an independent, MIT-licensed C11 library, installed as the
CMake package `QuirewisePDG`. Its public headers are `pdg.h`, `pdz.h` and
`pdz_pdf.h` at the include root, not under `quirewise/`. It can be consumed
without the Quirewise application.

## What it does

- Decrypts and decodes SSReader PDG pages: the private Group 4 dialect, JPEG,
  PNG, BMP, TIFF, DjVu, and native PDF/JP2 payloads that are returned verbatim.
- Renders a page to owned Gray8 or RGB24 pixels when the payload can be
  rasterised locally.
- Re-encodes bitonal pages as standard ITU-T T.6 (`pdg_render_to_ccitt`) so a
  PDF writer can embed them with `/CCITTFaxDecode`.
- Reads `%pdgz` PDZ containers: parses the header, decrypts the member table,
  extracts members with the bookkey or the fixed fallback key, decodes
  `pdg_type 0x1A` native-PDF members, and walks BKRT/inforule page rules.
- Writes PDFs from image pages, from standalone PDG files, or from a PDZ
  container, including a streaming writer with a caller-supplied byte sink.

It does not merge PDFs (`pdz_pdf_merge` is a documented `PDG_ERR_UNSUPPORTED`
placeholder) and does not rasterise PDF or JP2 payloads; those routes belong to
`Quirewise::Document`.

## Requirements

- C11 compiler and CMake 3.21 or newer.
- Installed Quirewise packages found with `find_package`: `QuirewiseRuntime`,
  `QuirewiseImage`, `QuirewiseCrypto` and `QuirewisePage`.
- No third-party libraries; CTest drives the tests.

## Build, test and install

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/prefix
```

Shared builds are available with `-DQUIREWISE_BUILD_SHARED=ON`.

## Use it from CMake

```cmake
find_package(QuirewisePDG CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE Quirewise::PDG)
```

Installed headers live at the include root, so `#include <pdg.h>` is the normal
form. Add `-DCMAKE_PREFIX_PATH=/path/to/prefix` for a non-default install.

## Quick start

Decode one PDG file and save it in its native encoding:

```c
#include <pdg.h>

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    PdgBuffer buffer = {0};
    char name[512];
    FILE *file;
    int status;

    if (argc != 3) return 1;
    status = pdg_decode_file(argv[1], &buffer);
    if (status != PDG_OK) {
        fprintf(stderr, "%s\n", pdg_error_string(status));
        return 1;
    }
    snprintf(name, sizeof(name), "%s.%s", argv[2],
             pdg_output_extension(buffer.kind));
    file = fopen(name, "wb");
    if (!file || fwrite(buffer.data, 1, buffer.size, file) != buffer.size) {
        if (file) fclose(file);
        pdg_buffer_free(&buffer);
        return 1;
    }
    fclose(file);
    printf("%s: %s, %u x %u, %zu bytes\n", name,
           pdg_output_kind_name(buffer.kind), buffer.header.width,
           buffer.header.height, buffer.size);
    pdg_buffer_free(&buffer);
    return 0;
}
```

Paths passed to the file entry points are UTF-8 on every platform.

## API overview

| Header | Purpose |
|---|---|
| `pdg.h` | PDG decrypt/decode/render, batches, T.6 re-encode, file entry points |
| `pdz.h` | PDZ header, members, keys, 0x1A PDF members, inforule, SSCT |
| `pdz_pdf.h` | PDF writer, page rules, streaming writer, PDF member helpers |

| Task | Functions |
|---|---|
| Version / errors | `pdg_abi_version`, `pdg_error_string`, `pdg_output_extension`, `pdg_output_kind_name`, `pdg_output_requires_external_decoder` |
| Decode | `pdg_decrypt_memory`, `pdg_decode_memory`, `pdg_decrypt_file`, `pdg_decode_file` |
| Render | `pdg_render_memory`, `pdg_render_to_ccitt`, `pdg_buffer_free`, `pdg_image_free` |
| Parallel batches | `pdg_decode_memory_batch`, `pdg_decrypt_memory_batch`, `pdg_render_memory_batch` |
| Context | `pdg_context_create`, `pdg_context_free`, `pdg_decode_memory_ctx`, `pdg_decrypt_memory_ctx`, `pdg_render_memory_ctx` |
| PDZ container | `pdz_is_container`, `pdz_parse`, `pdz_members`, `pdz_validate`, `pdz_member_payload`, `pdz_member_payload_ex`, `pdz_member_scope_for`, `pdz_member_scope_name`, `pdz_buffer_free` |
| PDZ 0x1A PDF | `pdz_pdfpdg_is_member`, `pdz_pdfpdg_decode_member`, `PDZ_PDFPDG_TYPE`, `PDZ_PDFPDG_HEADER_SIZE` |
| Inforule | `pdz_inforule_parse`, `pdz_inforule_rule_table`, `pdz_inforule_rule_member`, `pdz_inforule_page_member`, `pdz_inforule_member_pages` |
| PDF output | `pdz_pdf_from_images`, `pdz_pdf_from_pdg_files`, `pdz_pdf_from_pdz`, `pdz_pdf_free` |
| Streaming PDF | `pdz_pdf_writer_create`, `pdz_pdf_writer_add_page`, `pdz_pdf_writer_finish`, `pdz_pdf_writer_destroy` |
| PDF page rules | `pdz_pdf_resolve_rule`, `pdz_pdf_rule_count`, `pdz_pdf_member_selected`, `pdz_pdf_meta_pages`, `pdz_pdf_page_rule_name`, `pdz_pdf_route_name` |
| SSCT certificates | `pdz_ssct_decode`, `pdz_ssct_bookkey`, `pdz_ssct_bookkey_from_xml`, `pdz_ssct_solve_type2`, `pdz_ssct_hdd2key`, `pdz_derive_key`, `pdz_md5_hex_upper`, `pdz_aes256_decrypt_block` |
| Inflate | `pdz_inflate_raw`, `pdz_inflate_zlib` |

## Behavior and limits

- All status codes are negative on failure and `PDG_OK` is `0`. Some PDG errors
  (`PDG_ERR_NOT_PDG`, `PDG_ERR_NOT_PDZ`, `PDG_ERR_EMPTY`,
  `PDG_ERR_EXTERNAL_DECODER_REQUIRED`) have no per-format equivalent elsewhere.
- `pdg_decode_memory` returns the native encoding when the payload has one
  (JPEG, PNG, BMP, DjVu, TIFF, PDF, JP2) and a BMP otherwise; Group 4 pages are
  decoded to BMP. Direct image files passed to it are copied through.
- `pdg_render_memory` rasterises Group 4, BMP, JPEG, PNG, DjVu and TIFF. PDF and
  JP2 payloads return `PDG_ERR_EXTERNAL_DECODER_REQUIRED`.
- `pdg_render_to_ccitt` accepts Group 4 pages only. It emits horizontal-mode
  pairs: a valid, lossless T.6 stream roughly a third larger than an optimal
  encoder. JPEG/PNG/JP2 pages return `PDG_ERR_UNSUPPORTED`; embed those
  verbatim.
- JPEG and PNG rendering enforces a 32768-pixel dimension and 20,000,000-pixel
  cap before decoding; Group 4 output size comes from the PDG header.
- Batch calls clamp `jobs` to `1..PDG_MAX_PARALLEL_JOBS` (8). Per-item
  `status` is always written; the return value is the first error, or `PDG_OK`.
- `pdg_context_create` allocates optional per-thread Group 4 reference state.
  A context is not thread-safe; the batch APIs create one per worker. Passing
  `NULL` is always valid.
- PDZ member decryption defaults to the 1024-byte prefix scope. `bookType` is
  not a verified selector; pass `PDZ_MEMBER_SCOPE_WHOLE` only when the sample
  is known to need it.
- `pdz_pdf_from_pdz` reports `PDG_ERR_UNSUPPORTED` when the container stores
  native 0x1A PDF members, and fills `PdzPdfReport.pdf_members` so the caller
  can decode and merge them. `pdz_pdf_merge` is always unsupported.
- Buffers are owned by the value that returned them: `pdg_buffer_free`,
  `pdg_image_free`, `pdz_buffer_free`, `pdz_pdf_free`. Input bytes are borrowed
  and must stay alive for the call.
- No function renders PDF or JPEG 2000 payloads; those bytes are returned
  verbatim for another package to handle.

## Testing

`ctest` covers PDG decoding against Page's DjVu fixture, the fuzz smoke, the
parallel batch stress test, PDZ PDF geometry and image-mode tests, PDZ member
decoding, and Group 4 encoder goldens cross-checked with MuPDF.

## Documentation

See [docs/usage.md](docs/usage.md) for PDG/PDZ workflows, keys, page selection
and PDF writer recipes.

## License

MIT. See [LICENSE](LICENSE).
