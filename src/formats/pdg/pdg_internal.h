#ifndef PDG_INTERNAL_H
#define PDG_INTERNAL_H

#include "pdg.h"
#include "pdg_fax.h"

#include "../../core/byte_utils.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PDG_HEADER_SIZE 0x8c

typedef struct PdgByteView {
    const uint8_t *data;
    size_t size;
} PdgByteView;

typedef struct PdgDjvuReader PdgDjvuReader;

typedef struct PdgMutableBuffer {
    uint8_t *data;
    size_t size;
} PdgMutableBuffer;

struct PdgContext {
    int *fax_reference;
    int *fax_current;
    size_t fax_change_capacity;
};

int pdg_parse_header(const uint8_t *file_data, size_t file_size, PdgHeader *header);
PdgOutputKind pdg_detect_payload_kind(const uint8_t *data, size_t size);
void pdg_decode_dimensions(PdgHeader *header);

void pdg_md5(const uint8_t *data, size_t size, uint8_t digest[16]);
void pdg_repeating_xor(uint8_t *data, size_t size, const uint8_t *key, size_t key_size);
void pdg_pdg_block_decrypt(uint8_t block[16], const uint8_t key[16]);
void pdg_pdg_block_decrypt_buffer(uint8_t *data, size_t size, const uint8_t key[16]);
void pdg_xor_pi(uint8_t *data, size_t size);
int pdg_decrypt_payload_in_place(uint8_t *payload, size_t payload_size, const PdgHeader *header);

int pdg_apply_lightweight_scramble(const uint8_t *header, size_t header_size, uint8_t *payload, size_t payload_size, uint8_t type);
int pdg_apply_optional_layer(uint8_t *header, size_t header_size, uint8_t *payload, size_t payload_size);
int pdg_apply_a_family_transform(uint8_t *header, size_t header_size, uint8_t *payload, size_t payload_size, uint8_t type);
int pdg_apply_type28_transform(const uint8_t *header, size_t header_size, uint8_t *payload, size_t payload_size);

int pdg_decode_g4(const uint8_t *data, size_t size, int width, int height, uint8_t **bits_out, size_t *bits_size_out);
int pdg_bmp_from_1bpp(const uint8_t *bits, int width, int height, uint8_t **bmp_out, size_t *bmp_size_out);
int pdg_decode_g4_to_bmp(const uint8_t *data, size_t size, int width, int height, uint8_t **bmp_out, size_t *bmp_size_out);
int pdg_decode_g4_to_bmp_ctx(PdgContext *ctx,
                             const uint8_t *data,
                             size_t size,
                             int width,
                             int height,
                             uint8_t **bmp_out,
                             size_t *bmp_size_out);
int pdg_decode_g4_to_gray8_ctx(PdgContext *ctx,
                               const uint8_t *data,
                               size_t size,
                               int width,
                               int height,
                               PdgImage *out);

int pdg_render_tiff_payload(const uint8_t *data,
                            size_t size,
                            const PdgHeader *header,
                            PdgImage *out);
int pdg_render_decoded_payload_ctx(PdgContext *ctx,
                                   const PdgBuffer *payload,
                                   PdgImage *out);
int pdg_render_jp2_payload(const uint8_t *data,
                          size_t size,
                          const PdgHeader *header,
                          PdgImage *out);
int pdg_jp2_payload_dimensions(const uint8_t *data,
                               size_t size,
                               uint32_t *width,
                               uint32_t *height);
int pdg_jp2_payload_info(const uint8_t *data,
                         size_t size,
                         uint32_t *width,
                         uint32_t *height,
                         int *pdf_compatible,
                         int *smask_in_data);
int pdg_render_djvu_payload(const uint8_t *data,
                            size_t size,
                            const PdgHeader *header,
                            PdgImage *out);
int pdg_render_djvu_pages(const uint8_t *data,
                          size_t size,
                          const PdgHeader *header,
                          PdgImage **out_images,
                          size_t *out_count);
int pdg_djvu_reader_open(const uint8_t *data,
                         size_t size,
                         PdgDjvuReader **out_reader,
                         size_t *out_page_count);
int pdg_djvu_reader_render_page(PdgDjvuReader *reader,
                                size_t page_index,
                                const PdgHeader *header,
                                PdgImage *out);
void pdg_djvu_reader_close(PdgDjvuReader *reader);

void pdg_deslike_derive_schedules(const uint8_t key16[16], uint32_t schedules[6][32]);
void pdg_deslike_transform16(uint8_t block[16], const uint32_t schedules[6][32]);

extern const uint32_t pdg_des_round_sp[512];
extern const uint8_t pdg_des_key_rot[16];
extern const uint32_t pdg_des_key_pc[512];

#endif
