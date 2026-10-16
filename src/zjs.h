#ifndef P1600DN_ZJS_H
#define P1600DN_ZJS_H

#include <stdint.h>

typedef uint32_t DWORD;
typedef uint16_t WORD;
typedef uint8_t BYTE;

/* ZjStream chunk types of a P1600dn job. */
typedef enum {
    ZJT_START_DOC = 0,
    ZJT_END_DOC = 1,
    ZJT_START_PAGE = 2,
    ZJT_END_PAGE = 3,
    ZJT_JBIG_BIH = 4,
    ZJT_JBIG_BID = 5,
    ZJT_END_JBIG = 6,
    ZJT_PAUSE = 11          /* manual duplex: wait for the reloaded stack */
} ZJ_TYPE;

/* Big-endian 16-byte chunk header; `reserved` holds the item byte count. */
typedef struct {
    DWORD size;
    DWORD type;
    DWORD items;
    WORD reserved;
    WORD signature;
} ZJ_HEADER;

#define ZJ_SIGNATURE 0x5a5aU
#define ZJ_STREAM_SIGNATURE "JZJZ"
/* JBIG data goes out in chunks of exactly 64 KiB, padding only the last
 * one with zero bytes to a multiple of four. */
#define ZJ_BID_BLOCK_SIZE 65536U
#define ZJ_BID_PADDING 4U

/* ZjStream item ids used by the P1600dn. */
typedef enum {
    ZJI_PAGECOUNT = 0,
    ZJI_DMCOLLATE = 1,
    ZJI_DMDUPLEX = 2,
    ZJI_DMPAPER = 3,
    ZJI_DMCOPIES = 4,
    ZJI_DMDEFAULTSOURCE = 5,
    ZJI_DMMEDIATYPE = 6,
    ZJI_NBIE = 7,
    ZJI_RESOLUTION_X = 8,
    ZJI_RESOLUTION_Y = 9,
    ZJI_OFFSET_X = 10,
    ZJI_OFFSET_Y = 11,
    ZJI_RASTER_X = 12,
    ZJI_RASTER_Y = 13,
    ZJI_VIDEO_BPP = 16,
    ZJI_VIDEO_X = 17,
    ZJI_VIDEO_Y = 18,
    ZJI_RET = 22,
    ZJI_ECONOMODE = 23
} ZJ_ITEM;

/* Every automatically duplexed page carries DMDUPLEX 2 (long and short edge
 * alike); orientation is handled by page order and back-side rotation. */
#define ZJ_DMDUPLEX_AUTOMATIC 2U

typedef enum {
    DMBIN_MANUAL = 4,
    DMBIN_AUTO = 7
} DM_BIN;

/* DMMEDIATYPE values are the PPD's cupsMediaType codes, passed through. */
typedef enum {
    DMMEDIA_PLAIN = 1,
    DMMEDIA_TRANSPARENCY = 2,
    DMMEDIA_LIGHT = 258,
    DMMEDIA_BOND = 260,
    DMMEDIA_HEAVY = 262,
    DMMEDIA_ROUGH = 263,
    DMMEDIA_LABELS = 265,
    DMMEDIA_ENVELOPE = 267,
    DMMEDIA_VELLUM = 273,
    DMMEDIA_MID_WEIGHT = 282,
    DMMEDIA_EXTRA_HEAVY = 283,
    DMMEDIA_COLOR = 512,
    DMMEDIA_LETTERHEAD = 513,
    DMMEDIA_PREPRINTED = 514,
    DMMEDIA_PREPUNCHED = 515,
    DMMEDIA_RECYCLED = 516
} DMMEDIA;

typedef enum {
    DMPAPER_NONE = 0,               /* no paper code; unlisted sizes use DMPAPER_USER */
    DMPAPER_LETTER = 1,
    DMPAPER_LEGAL = 5,
    DMPAPER_EXECUTIVE = 7,
    DMPAPER_A4 = 9,
    DMPAPER_A5 = 11,
    DMPAPER_B5 = 13,
    DMPAPER_ENV_10 = 20,
    DMPAPER_ENV_DL = 27,
    DMPAPER_ENV_C5 = 28,
    DMPAPER_ENV_B5 = 34,
    DMPAPER_ENV_MONARCH = 37,
    DMPAPER_FOLIO_8_5X13 = 41,
    DMPAPER_POSTCARD = 43,
    DMPAPER_DOUBLE_POSTCARD = 69,
    DMPAPER_A6 = 70,
    DMPAPER_LETTER_ROTATED = 75,
    DMPAPER_A4_ROTATED = 77,
    DMPAPER_A5_ROTATED = 78,
    DMPAPER_B5_ROTATED = 79,
    DMPAPER_POSTCARD_ROTATED = 81,
    DMPAPER_DOUBLE_POSTCARD_ROTATED = 82,
    DMPAPER_A6_ROTATED = 83,
    DMPAPER_USER = 256,             /* custom size */
    DMPAPER_16K_197X273 = 257,
    DMPAPER_16K_184X260 = 263,
    DMPAPER_16K_195X270 = 264
} DMPAPER;

typedef enum {
    ZJIT_UINT32 = 1
} ZJ_ITEM_TYPE;

typedef struct {
    DWORD size;
    WORD item;
    BYTE type;
    BYTE param;
} ZJ_ITEM_HEADER;

typedef struct {
    ZJ_ITEM_HEADER header;
    DWORD value;
} ZJ_ITEM_UINT32;

static inline uint32_t be32(uint32_t dword) {
    uint32_t probe = 1;
    if (((const char *)&probe)[0] == 1) {
        return ((dword & 0x000000ffU) << 24) |
               ((dword & 0x0000ff00U) << 8) |
               ((dword & 0x00ff0000U) >> 8) |
               ((dword & 0xff000000U) >> 24);
    }
    return dword;
}

static inline uint16_t be16(uint16_t word) {
    uint16_t probe = 1;
    if (((const char *)&probe)[0] == 1) {
        return (uint16_t)(((word & 0x00ffU) << 8) | ((word & 0xff00U) >> 8));
    }
    return word;
}

#endif /* P1600DN_ZJS_H */
