#include <pdg.h>
#include <pdz.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FUZZ_SEED UINT32_C(0x51f0cc88)
#define FUZZ_RUNS 200u

static uint32_t rng_state = FUZZ_SEED;

static uint32_t next_random(void)
{
    uint32_t value = rng_state;
    value ^= value << 13u;
    value ^= value >> 17u;
    value ^= value << 5u;
    rng_state = value;
    return value;
}

int main(void)
{
    uint8_t buffer[2048];
    size_t run;
    size_t ok = 0u;
    size_t rejected = 0u;
    for (run = 0u; run < FUZZ_RUNS; ++run) {
        PdgBuffer decoded = {0};
        PdgImage image = {0};
        size_t size = 32u + (size_t)(next_random() % 1024u);
        size_t index;
        for (index = 0u; index < size; ++index)
            buffer[index] = (uint8_t)next_random();
        buffer[0] = 1u;
        buffer[1] = 1u;
        if (pdg_decode_memory(buffer, size, &decoded) == PDG_OK) {
            ++ok;
            pdg_buffer_free(&decoded);
        } else {
            ++rejected;
        }
        if (pdg_decrypt_memory(buffer, size, &decoded) == PDG_OK) {
            ++ok;
            pdg_buffer_free(&decoded);
        } else {
            ++rejected;
        }
        if (pdg_render_memory(buffer, size, &image) == PDG_OK) {
            ++ok;
            pdg_image_free(&image);
        } else {
            ++rejected;
        }
    }
    for (run = 0u; run < FUZZ_RUNS; ++run) {
        PdzInfo info = {0};
        size_t count = 0u;
        size_t size = 16u + (size_t)(next_random() % 1024u);
        size_t index;
        for (index = 0u; index < size; ++index)
            buffer[index] = (uint8_t)next_random();
        if (run % 2u == 0u) {
            memcpy(buffer, "%pdgz", 5u);
            if (size > 12u) {
                buffer[5] = 1u;
                buffer[6] = 0u;
                buffer[7] = 1u;
            }
        }
        (void)pdz_is_container(buffer, size);
        if (pdz_parse(buffer, size, &info) == PDG_OK)
            ++ok;
        else
            ++rejected;
        if (pdz_members(buffer, size, NULL, 0u, &count) == PDG_OK)
            ++ok;
        else
            ++rejected;
    }
    printf("PDG fuzz smoke: %zu ok, %zu rejected, seed=0x%08x runs=%u\n", ok,
           rejected, FUZZ_SEED, FUZZ_RUNS);
    return 0;
}
