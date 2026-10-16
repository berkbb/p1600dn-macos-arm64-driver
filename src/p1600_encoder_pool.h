#ifndef P1600_ENCODER_POOL_H
#define P1600_ENCODER_POOL_H

/*
 * Parallel page encoding for Apple Silicon. JBIG coding is serial within a
 * page but pages are independent images, so rendered pages are encoded on
 * worker threads (one per performance core, at most four) and handed back in
 * submission order. The bytes of every page are exactly those
 * zjs_write_page() writes; only the time they take changes. Every page
 * passes the page guard (zjs_guard.h) on its worker before it is handed
 * back; a page that fails it is never handed back.
 *
 * One thread submits pages, another takes them back; at most `depth` pages
 * (rendered or encoded) are held, so a slow printer holds the renderer back.
 */

#include <stdbool.h>
#include <stddef.h>

#include "zjs_encoder.h"

#define P1600_ENCODER_MAX_WORKERS 4U

typedef struct P1600EncoderPool P1600EncoderPool;

/* Encoder threads for this Mac: performance cores (hw.perflevel0), else
 * half the active cores, between 1 and P1600_ENCODER_MAX_WORKERS. The
 * environment variable P1600DN_ENCODER_THREADS (0-16) overrides it; 0
 * encodes on the submitting thread. */
unsigned p1600_encoder_default_workers(void);

/* `workers` threads (0 = encode inside p1600_encoder_pool_submit) and at most
 * `depth` pages held. NULL on failure. */
P1600EncoderPool *p1600_encoder_pool_create(unsigned workers, unsigned depth);

/* Queues a page, waiting while `depth` pages are held; the pool owns (and
 * frees) `bitmap`. Returns 0, or -1 after p1600_encoder_pool_close(). */
int p1600_encoder_pool_submit(P1600EncoderPool *pool, const ZjsPageOptions *page, unsigned char *bitmap);

/* No more pages will be submitted. */
void p1600_encoder_pool_close(P1600EncoderPool *pool);

/* Waits for the oldest page. Returns 1 with its ZjStream bytes in `*data`
 * (free() them), -1 when that page could not be encoded or failed the page
 * guard, and 0 once the pool is closed and every page was taken. */
int p1600_encoder_pool_next(P1600EncoderPool *pool, unsigned char **data, size_t *length);

/* Why the page p1600_encoder_pool_next() last returned -1 for failed ("" when
 * none failed). Valid until the next p1600_encoder_pool_next(). */
const char *p1600_encoder_pool_error(const P1600EncoderPool *pool);

/* Stops the workers and frees everything still held. */
void p1600_encoder_pool_destroy(P1600EncoderPool *pool);

#endif /* P1600_ENCODER_POOL_H */
