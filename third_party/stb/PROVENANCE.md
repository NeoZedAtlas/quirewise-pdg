# Vendored source record: stb_image (PDG copy)

- Component: `stb_image.h`, single-header image decoder.
- Version: **v2.30** (same bytes as the copy reviewed in
  `libraries/quirewise-image/third_party/stb`; line 1 states the version).
- Origin: <https://github.com/nothings/stb> (Sean Barrett / nothings.org).
- License: public domain (upstream also offers MIT).
- Why present: `src/formats/pdg/pdg_render.c` includes `stb_image.h` to decode
  embedded page images while rendering PDG/DjVu pages.
- Patches: none; header byte-identical to upstream v2.30.
- Ownership: third-party code, private include directory only, never installed
  and never referenced by a public header.
