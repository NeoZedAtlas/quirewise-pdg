#include "pdg_internal.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
typedef HANDLE PdgThread;
typedef CRITICAL_SECTION PdgMutex;
static void pdg_mutex_init(PdgMutex *mutex) { InitializeCriticalSection(mutex); }
static void pdg_mutex_destroy(PdgMutex *mutex) { DeleteCriticalSection(mutex); }
static void pdg_mutex_lock(PdgMutex *mutex) { EnterCriticalSection(mutex); }
static void pdg_mutex_unlock(PdgMutex *mutex) { LeaveCriticalSection(mutex); }
#else
#include <pthread.h>
typedef pthread_t PdgThread;
typedef pthread_mutex_t PdgMutex;
static void pdg_mutex_init(PdgMutex *mutex) { pthread_mutex_init(mutex, NULL); }
static void pdg_mutex_destroy(PdgMutex *mutex) { pthread_mutex_destroy(mutex); }
static void pdg_mutex_lock(PdgMutex *mutex) { pthread_mutex_lock(mutex); }
static void pdg_mutex_unlock(PdgMutex *mutex) { pthread_mutex_unlock(mutex); }
#endif

typedef struct PdgBatchState {
    void *items;
    size_t count;
    size_t next_index;
    int first_error;
    int mode;
    PdgMutex mutex;
} PdgBatchState;

enum {
    PDG_BATCH_DECRYPT = 0,
    PDG_BATCH_DECODE = 1,
    PDG_BATCH_RENDER = 2
};

static size_t clamp_jobs(size_t jobs, size_t count)
{
    if (count == 0) {
        return 0;
    }
    if (jobs == 0) {
        jobs = 1;
    }
    if (jobs > count) {
        jobs = count;
    }
    if (jobs > PDG_MAX_PARALLEL_JOBS) {
        jobs = PDG_MAX_PARALLEL_JOBS;
    }
    return jobs;
}

static size_t take_next_index(PdgBatchState *state)
{
    pdg_mutex_lock(&state->mutex);
    size_t index = state->next_index;
    if (index < state->count) {
        state->next_index += 1;
    }
    pdg_mutex_unlock(&state->mutex);
    return index;
}

static void record_error(PdgBatchState *state, int rc)
{
    if (rc == PDG_OK) {
        return;
    }
    pdg_mutex_lock(&state->mutex);
    if (state->first_error == PDG_OK) {
        state->first_error = rc;
    }
    pdg_mutex_unlock(&state->mutex);
}

static void batch_worker_run(PdgBatchState *state)
{
    PdgContext *ctx = pdg_context_create();
    if (!ctx) {
        for (;;) {
            size_t index = take_next_index(state);
            if (index >= state->count) {
                break;
            }
            if (state->mode == PDG_BATCH_RENDER) {
                PdgRenderItem *items = (PdgRenderItem *)state->items;
                items[index].status = PDG_ERR_OUT_OF_MEMORY;
                memset(&items[index].image, 0, sizeof(items[index].image));
            } else {
                PdgBatchItem *items = (PdgBatchItem *)state->items;
                items[index].status = PDG_ERR_OUT_OF_MEMORY;
                memset(&items[index].output, 0, sizeof(items[index].output));
            }
        }
        record_error(state, PDG_ERR_OUT_OF_MEMORY);
        return;
    }

    for (;;) {
        size_t index = take_next_index(state);
        if (index >= state->count) {
            break;
        }

        if (state->mode == PDG_BATCH_RENDER) {
            PdgRenderItem *items = (PdgRenderItem *)state->items;
            PdgRenderItem *item = &items[index];
            memset(&item->image, 0, sizeof(item->image));
            int rc = pdg_render_memory_ctx(ctx, item->data, item->size, &item->image);
            item->status = rc;
            if (rc != PDG_OK) {
                memset(&item->image, 0, sizeof(item->image));
                record_error(state, rc);
            }
        } else {
            PdgBatchItem *items = (PdgBatchItem *)state->items;
            PdgBatchItem *item = &items[index];
            memset(&item->output, 0, sizeof(item->output));
            int rc = state->mode == PDG_BATCH_DECODE ? pdg_decode_memory_ctx(ctx, item->data, item->size, &item->output)
                                                     : pdg_decrypt_memory_ctx(ctx, item->data, item->size, &item->output);
            item->status = rc;
            if (rc != PDG_OK) {
                memset(&item->output, 0, sizeof(item->output));
                record_error(state, rc);
            }
        }
    }

    pdg_context_free(ctx);
}

#if defined(_WIN32)
static DWORD WINAPI batch_thread_main(LPVOID param)
{
    batch_worker_run((PdgBatchState *)param);
    return 0;
}

static int create_thread(PdgThread *thread, PdgBatchState *state)
{
    *thread = CreateThread(NULL, 0, batch_thread_main, state, 0, NULL);
    return *thread ? PDG_OK : PDG_ERR_OUT_OF_MEMORY;
}

static void join_thread(PdgThread thread)
{
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
}
#else
static void *batch_thread_main(void *param)
{
    batch_worker_run((PdgBatchState *)param);
    return NULL;
}

static int create_thread(PdgThread *thread, PdgBatchState *state)
{
    return pthread_create(thread, NULL, batch_thread_main, state) == 0 ? PDG_OK : PDG_ERR_OUT_OF_MEMORY;
}

static void join_thread(PdgThread thread)
{
    pthread_join(thread, NULL);
}
#endif

static int run_memory_batch(PdgBatchItem *items, size_t count, size_t jobs, int mode)
{
    if (!items && count != 0) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    if (count == 0) {
        return PDG_OK;
    }

    for (size_t i = 0; i < count; ++i) {
        memset(&items[i].output, 0, sizeof(items[i].output));
        items[i].status = PDG_OK;
    }

    jobs = clamp_jobs(jobs, count);
    if (jobs <= 1u) {
        PdgBatchState state;
        memset(&state, 0, sizeof(state));
        state.items = items;
        state.count = count;
        state.mode = mode;
        pdg_mutex_init(&state.mutex);
        batch_worker_run(&state);
        pdg_mutex_destroy(&state.mutex);
        return state.first_error;
    }

    PdgBatchState state;
    memset(&state, 0, sizeof(state));
    state.items = items;
    state.count = count;
    state.mode = mode;
    pdg_mutex_init(&state.mutex);

    PdgThread *threads = (PdgThread *)calloc(jobs, sizeof(*threads));
    if (!threads) {
        pdg_mutex_destroy(&state.mutex);
        return PDG_ERR_OUT_OF_MEMORY;
    }

    size_t created = 0;
    for (; created < jobs; ++created) {
        int rc = create_thread(&threads[created], &state);
        if (rc != PDG_OK) {
            record_error(&state, rc);
            break;
        }
    }
    if (created == 0) {
        batch_worker_run(&state);
    }
    for (size_t i = 0; i < created; ++i) {
        join_thread(threads[i]);
    }

    free(threads);
    int first_error = state.first_error;
    pdg_mutex_destroy(&state.mutex);
    return first_error;
}

PDG_API int pdg_decode_memory_batch(PdgBatchItem *items, size_t count, size_t jobs)
{
    return run_memory_batch(items, count, jobs, PDG_BATCH_DECODE);
}

PDG_API int pdg_decrypt_memory_batch(PdgBatchItem *items, size_t count, size_t jobs)
{
    return run_memory_batch(items, count, jobs, PDG_BATCH_DECRYPT);
}

PDG_API int pdg_render_memory_batch(PdgRenderItem *items, size_t count, size_t jobs)
{
    if (!items && count != 0) {
        return PDG_ERR_INVALID_ARGUMENT;
    }
    if (count == 0) {
        return PDG_OK;
    }

    for (size_t i = 0; i < count; ++i) {
        memset(&items[i].image, 0, sizeof(items[i].image));
        items[i].status = PDG_OK;
    }

    jobs = clamp_jobs(jobs, count);
    PdgBatchState state;
    memset(&state, 0, sizeof(state));
    state.items = items;
    state.count = count;
    state.mode = PDG_BATCH_RENDER;
    pdg_mutex_init(&state.mutex);

    if (jobs <= 1u) {
        batch_worker_run(&state);
        int first_error = state.first_error;
        pdg_mutex_destroy(&state.mutex);
        return first_error;
    }

    PdgThread *threads = (PdgThread *)calloc(jobs, sizeof(*threads));
    if (!threads) {
        pdg_mutex_destroy(&state.mutex);
        return PDG_ERR_OUT_OF_MEMORY;
    }

    size_t created = 0;
    for (; created < jobs; ++created) {
        int rc = create_thread(&threads[created], &state);
        if (rc != PDG_OK) {
            record_error(&state, rc);
            break;
        }
    }
    if (created == 0) {
        batch_worker_run(&state);
    }
    for (size_t i = 0; i < created; ++i) {
        join_thread(threads[i]);
    }

    free(threads);
    int first_error = state.first_error;
    pdg_mutex_destroy(&state.mutex);
    return first_error;
}
