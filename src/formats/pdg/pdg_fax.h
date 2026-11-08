#ifndef PDG_FAX_H
#define PDG_FAX_H

#include <stddef.h>
#include <stdint.h>

typedef struct PdgPrefixCode {
    uint16_t bits;
    uint8_t length;
    int value;
} PdgPrefixCode;

extern const PdgPrefixCode pdg_fax_white_codes[];
extern const size_t pdg_fax_white_code_count;
extern const PdgPrefixCode pdg_fax_black_codes[];
extern const size_t pdg_fax_black_code_count;
extern const PdgPrefixCode pdg_fax_mode_codes[];
extern const size_t pdg_fax_mode_code_count;

int qw_pdg_decode_g4(const uint8_t *data,
                  size_t size,
                  int width,
                  int height,
                  uint8_t **bits_out,
                  size_t *bits_size_out);

/* Encode packed 1-bit rows (MSB first, 1 = black) as ITU-T T.6. The stream is
 * built from horizontal-mode pairs only, so it trades ~3x size for a simple
 * decode-side state machine. */
int pdg_encode_g4_horizontal(const uint8_t *bits,
                             size_t bits_size,
                             uint32_t width,
                             uint32_t height,
                             uint8_t **out_data,
                             size_t *out_size);

#endif
