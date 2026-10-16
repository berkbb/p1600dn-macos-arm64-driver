#include "p1600_encoder_pool.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

#include "zjs_guard.h"

typedef enum { SLOT_EMPTY = 0, SLOT_QUEUED, SLOT_ENCODING, SLOT_DONE, SLOT_FAILED } SlotState;

typedef struct {
    SlotState state;
    ZjsPageOptions page;
    unsigned char *bitmap;
    unsigned char *data;
    size_t length;
    char reason[192];        /* why the page failed */
} Slot;

struct P1600EncoderPool {
    pthread_mutex_t lock;
    pthread_cond_t work;     /* a page was queued, or shutdown */
    pthread_cond_t done;     /* a page finished, or the pool was closed */
    pthread_cond_t space;    /* a page was taken back */
    pthread_t threads[P1600_ENCODER_MAX_WORKERS];
    unsigned workers;
    unsigned depth;
    Slot *slots;             /* ring indexed by sequence % depth */
    uint64_t submitted;      /* sequence of the next submitted page */
    uint64_t encoding;       /* sequence of the next page a worker takes */
    uint64_t emitted;        /* sequence of the next page handed back */
    bool closed;
    bool shutdown;
    char error[192];         /* reason of the last failed page handed back */
};

static unsigned sysctl_count(const char *name) {
    int value = 0;
    size_t size = sizeof(value);
    return sysctlbyname(name, &value, &size, NULL, 0) == 0 && value > 0 ? (unsigned)value : 0U;
}

unsigned p1600_encoder_default_workers(void) {
    const char *override = getenv("P1600DN_ENCODER_THREADS");
    if (override && *override) {
        char *end = NULL;
        errno = 0;
        unsigned long value = strtoul(override, &end, 10);
        if (end != override && *end == '\0' && errno == 0 && value <= 16UL) {
            return value > P1600_ENCODER_MAX_WORKERS ? P1600_ENCODER_MAX_WORKERS : (unsigned)value;
        }
    }
    unsigned cores = sysctl_count("hw.perflevel0.physicalcpu");
    if (cores == 0) cores = sysctl_count("hw.activecpu") / 2U;
    if (cores == 0) cores = 1;
    return cores > P1600_ENCODER_MAX_WORKERS ? P1600_ENCODER_MAX_WORKERS : cores;
}

/* zjs_write_page() into memory, proven by the page guard. */
static int encode(Slot *slot) {
    return zjs_encode_page_checked(&slot->page, slot->bitmap, &slot->data, &slot->length, slot->reason,
                                   sizeof(slot->reason));
}

static void finish_slot(Slot *slot, int status) {
    free(slot->bitmap);
    slot->bitmap = NULL;
    slot->state = status == 0 ? SLOT_DONE : SLOT_FAILED;
}

static void *worker_main(void *argument) {
    P1600EncoderPool *pool = argument;
    pthread_mutex_lock(&pool->lock);
    for (;;) {
        while (!pool->shutdown && pool->encoding == pool->submitted) {
            pthread_cond_wait(&pool->work, &pool->lock);
        }
        if (pool->shutdown) break;
        Slot *slot = &pool->slots[pool->encoding % pool->depth];
        pool->encoding++;
        slot->state = SLOT_ENCODING;
        pthread_mutex_unlock(&pool->lock);
        int status = encode(slot);
        pthread_mutex_lock(&pool->lock);
        finish_slot(slot, status);
        pthread_cond_broadcast(&pool->done);
    }
    pthread_mutex_unlock(&pool->lock);
    return NULL;
}

P1600EncoderPool *p1600_encoder_pool_create(unsigned workers, unsigned depth) {
    if (workers > P1600_ENCODER_MAX_WORKERS || depth == 0) return NULL;
    P1600EncoderPool *pool = calloc(1, sizeof(*pool));
    if (!pool) return NULL;
    pool->slots = calloc(depth, sizeof(*pool->slots));
    pool->depth = depth;
    if (!pool->slots || pthread_mutex_init(&pool->lock, NULL) != 0) {
        free(pool->slots);
        free(pool);
        return NULL;
    }
    if (pthread_cond_init(&pool->work, NULL) != 0 || pthread_cond_init(&pool->done, NULL) != 0 ||
        pthread_cond_init(&pool->space, NULL) != 0) {
        pthread_mutex_destroy(&pool->lock);
        free(pool->slots);
        free(pool);
        return NULL;
    }
    for (; pool->workers < workers; pool->workers++) {
        if (pthread_create(&pool->threads[pool->workers], NULL, worker_main, pool) != 0) {
            p1600_encoder_pool_destroy(pool);
            return NULL;
        }
    }
    return pool;
}

int p1600_encoder_pool_submit(P1600EncoderPool *pool, const ZjsPageOptions *page, unsigned char *bitmap) {
    if (!pool || !page || !bitmap) {
        free(bitmap);
        return -1;
    }
    pthread_mutex_lock(&pool->lock);
    while (!pool->closed && pool->submitted - pool->emitted >= pool->depth) {
        pthread_cond_wait(&pool->space, &pool->lock);
    }
    if (pool->closed) {
        pthread_mutex_unlock(&pool->lock);
        free(bitmap);
        return -1;
    }
    Slot *slot = &pool->slots[pool->submitted % pool->depth];
    *slot = (Slot){.state = SLOT_QUEUED, .page = *page, .bitmap = bitmap};
    pool->submitted++;
    if (pool->workers == 0) {
        pool->encoding++;
        finish_slot(slot, encode(slot));
        pthread_cond_broadcast(&pool->done);
    } else {
        pthread_cond_signal(&pool->work);
    }
    pthread_mutex_unlock(&pool->lock);
    return 0;
}

void p1600_encoder_pool_close(P1600EncoderPool *pool) {
    if (!pool) return;
    pthread_mutex_lock(&pool->lock);
    pool->closed = true;
    pthread_cond_broadcast(&pool->done);
    pthread_cond_broadcast(&pool->space);
    pthread_mutex_unlock(&pool->lock);
}

int p1600_encoder_pool_next(P1600EncoderPool *pool, unsigned char **data, size_t *length) {
    if (!pool || !data || !length) return -1;
    *data = NULL;
    *length = 0;
    pthread_mutex_lock(&pool->lock);
    for (;;) {
        if (pool->emitted < pool->submitted && pool->slots[pool->emitted % pool->depth].state >= SLOT_DONE) break;
        if (pool->emitted == pool->submitted && pool->closed) {
            pthread_mutex_unlock(&pool->lock);
            return 0;
        }
        pthread_cond_wait(&pool->done, &pool->lock);
    }
    Slot *slot = &pool->slots[pool->emitted % pool->depth];
    int result = slot->state == SLOT_DONE ? 1 : -1;
    if (result < 0) snprintf(pool->error, sizeof(pool->error), "%s", slot->reason);
    *data = slot->data;
    *length = slot->length;
    *slot = (Slot){.state = SLOT_EMPTY};
    pool->emitted++;
    pthread_cond_signal(&pool->space);
    pthread_mutex_unlock(&pool->lock);
    return result;
}

const char *p1600_encoder_pool_error(const P1600EncoderPool *pool) {
    return pool ? pool->error : "";
}

void p1600_encoder_pool_destroy(P1600EncoderPool *pool) {
    if (!pool) return;
    pthread_mutex_lock(&pool->lock);
    pool->shutdown = true;
    pthread_cond_broadcast(&pool->work);
    pthread_mutex_unlock(&pool->lock);
    for (unsigned i = 0; i < pool->workers; i++) pthread_join(pool->threads[i], NULL);
    for (unsigned i = 0; i < pool->depth; i++) {
        free(pool->slots[i].bitmap);
        free(pool->slots[i].data);
    }
    pthread_cond_destroy(&pool->space);
    pthread_cond_destroy(&pool->done);
    pthread_cond_destroy(&pool->work);
    pthread_mutex_destroy(&pool->lock);
    free(pool->slots);
    free(pool);
}
