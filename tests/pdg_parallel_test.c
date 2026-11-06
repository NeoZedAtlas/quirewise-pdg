#include <pdg.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if !defined(_WIN32)
#include <pthread.h>
#endif

#define ITEM_COUNT 8u
#define THREAD_ITERATIONS 50u

static void fill_items(PdgBatchItem *items, uint8_t data[ITEM_COUNT][64])
{
    size_t index;
    for (index = 0u; index < ITEM_COUNT; ++index) {
        size_t byte;
        for (byte = 0u; byte < 64u; ++byte)
            data[index][byte] = (uint8_t)(index * 37u + byte * 11u);
        memset(&items[index], 0, sizeof(items[index]));
        items[index].data = data[index];
        items[index].size = 64u;
    }
}

static void free_outputs(PdgBatchItem *items)
{
    size_t index;
    for (index = 0u; index < ITEM_COUNT; ++index) {
        if (items[index].output.data) pdg_buffer_free(&items[index].output);
    }
}

static int batch_round(void)
{
    PdgBatchItem items[ITEM_COUNT];
    uint8_t data[ITEM_COUNT][64];
    fill_items(items, data);
    (void)pdg_decode_memory_batch(items, ITEM_COUNT, 4u);
    free_outputs(items);
    fill_items(items, data);
    (void)pdg_decrypt_memory_batch(items, ITEM_COUNT, 3u);
    free_outputs(items);
    return 0;
}

#if !defined(_WIN32)
static void *thread_main(void *argument)
{
    size_t iteration;
    (void)argument;
    for (iteration = 0u; iteration < THREAD_ITERATIONS; ++iteration) {
        if (batch_round() != 0) return (void *)1;
    }
    return NULL;
}
#endif

int main(void)
{
    static const uint8_t valid_header[8] = {1u, 1u, 0u, 0u, 0u, 0u, 0u, 0u};
    PdgBatchItem items[ITEM_COUNT];
    uint8_t data[ITEM_COUNT][64];
    size_t index;
    fill_items(items, data);
    memcpy(data[0], valid_header, sizeof(valid_header));
    items[0].data = data[0];
    (void)pdg_decode_memory_batch(items, ITEM_COUNT, 4u);
    free_outputs(items);
#if !defined(_WIN32)
    {
        pthread_t threads[4];
        size_t created;
        int failures = 0;
        for (created = 0u; created < 4u; ++created) {
            if (pthread_create(&threads[created], NULL, thread_main, NULL) != 0)
                return 2;
        }
        for (index = 0u; index < 4u; ++index) {
            void *result = NULL;
            (void)pthread_join(threads[index], &result);
            if (result != NULL) ++failures;
        }
        if (failures) {
            fprintf(stderr, "parallel thread failures: %d\n", failures);
            return 3;
        }
    }
#endif
    puts("PDG parallel batch stress passed");
    return 0;
}
