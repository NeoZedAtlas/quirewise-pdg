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

#endif
