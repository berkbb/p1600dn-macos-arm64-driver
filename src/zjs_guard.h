#ifndef ZJS_GUARD_H
#define ZJS_GUARD_H

/*
 * Page guard: no page reaches the printer unless it is provably the page
 * the printer's firmware expects.
 *
 * The black stripes the P1606dn printed across the bottom of pages came from
 * pages whose JBIG image and ZjStream geometry disagreed (VIDEO_Y not a
 * 12-line multiple, odd RASTER_X, JBIG Yd padded past VIDEO_Y). Every page
 * is therefore encoded into memory and proven before it is sent:
 *   - white padding: the lines between the content and VIDEO_Y and the bits
 *     right of RASTER_X are cleared, so nothing can print there;
 *   - geometry: VIDEO_Y a multiple of 12 ending within 12 lines of the
 *     content, RASTER_X = VIDEO_X * VIDEO_BPP, RASTER_Y = VIDEO_Y, JBIG
 *     Xd = the line bits, Yd = VIDEO_Y, the printer's L0, order and options;
 *   - framing: the START_PAGE items in the printer's order, BID chunks of
 *     exactly 64 KiB except the last (4-byte padded), END_JBIG, END_PAGE,
 *     nothing else;
 *   - content: the JBIG data decodes to exactly the bitmap, every pixel.
 * A page that fails is never sent.
 */

#include <stddef.h>

#include "zjs_encoder.h"

/* Clears the lines from page->content_height to page->height and the bits
 * past width * bpp in every line. */
void zjs_guard_clear_padding(const ZjsPageOptions *page, unsigned char *bitmap);

/* Checks one encoded page (zjs_write_page() output) against its options and
 * bitmap. Returns 0, or -1 with the reason. */
int zjs_guard_check(const unsigned char *data, size_t length, const ZjsPageOptions *page,
                    const unsigned char *bitmap, char *reason, size_t reason_size);

/* Clears the padding, encodes the page into memory and checks it. Returns 0
 * with the page bytes in *data (malloc'd, the caller frees them), or -1 with
 * the reason. */
int zjs_encode_page_checked(const ZjsPageOptions *page, unsigned char *bitmap,
                            unsigned char **data, size_t *length, char *reason, size_t reason_size);

#endif /* ZJS_GUARD_H */
