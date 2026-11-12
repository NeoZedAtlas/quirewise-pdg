#ifndef PDG_H
#define PDG_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(PDG_BUILD_SHARED)
#    if defined(PDG_EXPORTS)
#      define PDG_API __declspec(dllexport)
#    else
#      define PDG_API __declspec(dllimport)
#    endif
#  else
#    define PDG_API
#  endif
#elif defined(PDG_BUILD_SHARED)
#  define PDG_API __attribute__((visibility("default")))
#else
#  define PDG_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define PDG_ABI_VERSION 8
/* Upper bound for worker threads created by the memory-batch APIs. */
#define PDG_MAX_PARALLEL_JOBS 8u

enum {
    PDG_OK = 0,
    PDG_ERR_INVALID_ARGUMENT = -1,
    PDG_ERR_OUT_OF_MEMORY = -2,
    PDG_ERR_IO = -3,
    PDG_ERR_FORMAT = -4,
    PDG_ERR_UNSUPPORTED = -5,
    PDG_ERR_DECODE = -6,
    PDG_ERR_NOT_PDG = -7,
    PDG_ERR_EMPTY = -8,
    PDG_ERR_EXTERNAL_DECODER_REQUIRED = -9,
    PDG_ERR_NOT_PDZ = -10,
    PDG_ERR_RESOURCE_LIMIT = -11
};

typedef enum PdgOutputKind {
    PDG_OUTPUT_RAW = 0,
    PDG_OUTPUT_BMP,
    PDG_OUTPUT_JPEG,
    PDG_OUTPUT_PNG,
    PDG_OUTPUT_DJVU,
    PDG_OUTPUT_TIFF,
    PDG_OUTPUT_PDF,
    PDG_OUTPUT_JP2,
    /* Standard ITU-T T.6 bitstream produced by pdg_render_to_ccitt(). */
    PDG_OUTPUT_CCITT
} PdgOutputKind;

typedef enum PdgPixelFormat {
    PDG_PIXEL_GRAY8 = 1,
    PDG_PIXEL_RGB24 = 2
} PdgPixelFormat;

typedef struct PdgHeader {
    uint8_t version;
    uint8_t type;
    uint16_t width_cipher;
    uint16_t height_cipher;
    uint16_t width;
    uint16_t height;
    uint8_t color_depth;
    uint8_t layer_count;
    uint32_t data_offset;
    uint32_t data_size;
    uint8_t key_data[0x30];
    uint8_t ss_user_key[4];
} PdgHeader;

typedef struct PdgBuffer {
    uint8_t *data;
    size_t size;
    PdgOutputKind kind;
    PdgHeader header;
} PdgBuffer;

typedef struct PdgBatchItem {
    const void *data;
    size_t size;
    PdgBuffer output;
    int status;
} PdgBatchItem;

typedef struct PdgImage {
    uint8_t *pixels;
    size_t size;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    PdgPixelFormat format;
    PdgHeader header;
} PdgImage;

typedef struct PdgRenderItem {
    const void *data;
    size_t size;
    PdgImage image;
    int status;
} PdgRenderItem;

typedef struct PdgContext PdgContext;

PDG_API int pdg_abi_version(void);

PDG_API PdgContext *pdg_context_create(void);
PDG_API void pdg_context_free(PdgContext *ctx);

PDG_API int pdg_decode_memory(const void *data, size_t size, PdgBuffer *out);
PDG_API int pdg_decrypt_memory(const void *data, size_t size, PdgBuffer *out);
PDG_API int pdg_decode_memory_ctx(PdgContext *ctx, const void *data, size_t size, PdgBuffer *out);
PDG_API int pdg_decrypt_memory_ctx(PdgContext *ctx, const void *data, size_t size, PdgBuffer *out);
PDG_API int pdg_decode_memory_batch(PdgBatchItem *items, size_t count, size_t jobs);
PDG_API int pdg_decrypt_memory_batch(PdgBatchItem *items, size_t count, size_t jobs);
/* PDF/JP2 payloads cannot be rasterised inside this package: pdg_render_memory()
 * returns PDG_ERR_EXTERNAL_DECODER_REQUIRED. The application composition layer
 * renders PDF page payloads with the internal quirewise-document engine. */
PDG_API int pdg_render_memory(const void *data, size_t size, PdgImage *out);
PDG_API int pdg_render_memory_ctx(PdgContext *ctx, const void *data, size_t size, PdgImage *out);
PDG_API int pdg_render_memory_batch(PdgRenderItem *items, size_t count, size_t jobs);
PDG_API int pdg_decode_file(const char *path, PdgBuffer *out);
PDG_API int pdg_decrypt_file(const char *path, PdgBuffer *out);

/*!
 * Re-encode a bitonal PDG page as a standard ITU-T T.6 (CCITT Group 4)
 * bitstream. The page keeps its exact pixels; only the container changes, so a
 * PDF writer can embed the result with /CCITTFaxDecode instead of storing one
 * byte per pixel. SSReader's private Group 4 variant (type 0x00 and the
 * 0x02/0x03/0x1x families) is decoded first through the regular decoder.
 *
 * JPEG, PNG and JPEG 2000 pages already carry their own encoding and report
 * PDG_ERR_UNSUPPORTED: embed those verbatim. The output is owned by the caller
 * and released with pdg_buffer_free(); out->header reports the pixel size.
 *
 * Paths passed to pdg_decode_file/pdg_decrypt_file are UTF-8, including on
 * Windows.
 */
PDG_API int pdg_render_to_ccitt(const void *data, size_t size, PdgBuffer *out);
PDG_API void pdg_buffer_free(PdgBuffer *buffer);
PDG_API void pdg_image_free(PdgImage *image);

PDG_API const char *pdg_error_string(int code);
PDG_API const char *pdg_output_extension(PdgOutputKind kind);
PDG_API const char *pdg_output_kind_name(PdgOutputKind kind);
PDG_API int pdg_output_requires_external_decoder(PdgOutputKind kind);

#ifdef __cplusplus
}
#endif

#endif
