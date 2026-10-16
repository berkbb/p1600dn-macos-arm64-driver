#include "p1600_screen.h"

#include <pthread.h>
#include <string.h>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include "p1600_screen_tables.h"

/* FastRes 1200, 2 bits: level = cell >> (6 - 2 * (x & 3)), where the cell
 * byte for darkness g, screen row r and column group (x / 4) % 3 sits at
 * g * 48 + r * 4 + group. */
static void screen_two_bit(const uint8_t *gray, uint32_t start, uint32_t width,
                           uint32_t remaining, uint8_t *out) {
    const unsigned row = remaining % 12U;
    for (uint32_t x = start; x < width; x++) {
        unsigned darkness = 255U - gray[x];
        if (darkness == 0U) continue;
        unsigned shift = 6U - 2U * (x & 3U);
        unsigned cell = p1600_screen_2bpp[darkness * 48U + row * 4U + (x / 4U) % 3U];
        out[x >> 2] |= (uint8_t)(((cell >> shift) & 3U) << shift);
    }
}

/* 600 dpi, 1 bit: black when darkness reaches the 16x16 threshold. */
static void screen_one_bit(const uint8_t *gray, uint32_t start, uint32_t width,
                           uint32_t remaining, uint8_t *out) {
    const unsigned char *thresholds = p1600_screen_1bpp + (remaining % 16U) * 16U;
    for (uint32_t x = start; x < width; x++) {
        unsigned darkness = 255U - gray[x];
        if (darkness != 0U && darkness >= thresholds[x % 16U]) {
            out[x >> 3] |= (uint8_t)(0x80U >> (x & 7U));
        }
    }
}

void p1600_screen_line_scalar(const uint8_t *gray, uint32_t width, uint32_t bpp,
                           uint32_t remaining, uint8_t *out) {
    if (!gray || !out) return;
    if (bpp == 2U) {
        screen_two_bit(gray, 0, width, remaining, out);
    } else {
        screen_one_bit(gray, 0, width, remaining, out);
    }
}

#if defined(__ARM_NEON)
/*
 * Every cell of the FastRes table rises monotonically with darkness, so a
 * cell's level is the number of its three steps the darkness has passed:
 * level = (g > t1) + (g > t2) + (g > t3) with t = first darkness of the step
 * minus one (255 when the step is never reached). The 12-pixel screen period
 * repeats every 48 pixels, three 16-lane NEON vectors.
 */
static uint8_t step_thresholds[12][3][3][16]; /* [row][vector][step][lane] */
static bool vector_screen = false;
static pthread_once_t thresholds_once = PTHREAD_ONCE_INIT;

static void build_thresholds(void) {
    for (unsigned row = 0; row < 12U; row++) {
        for (unsigned pixel = 0; pixel < 48U; pixel++) {
            unsigned column = pixel % 12U;
            unsigned group = column / 4U, shift = 6U - 2U * (column % 4U);
            unsigned level = 0;
            unsigned steps[3] = {255U, 255U, 255U};
            for (unsigned darkness = 1; darkness < 256U; darkness++) {
                unsigned now = (p1600_screen_2bpp[darkness * 48U + row * 4U + group] >> shift) & 3U;
                if (now < level) return; /* not monotonic: keep the scalar screen */
                while (level < now) steps[level++] = darkness - 1U;
            }
            for (unsigned step = 0; step < 3U; step++) {
                step_thresholds[row][pixel / 16U][step][pixel % 16U] = (uint8_t)steps[step];
            }
        }
    }
    vector_screen = true;
}

static uint32_t screen_two_bit_neon(const uint8_t *gray, uint32_t width,
                                    uint32_t remaining, uint8_t *out) {
    const uint8_t (*rows)[3][16] = step_thresholds[remaining % 12U];
    uint8x16_t steps[3][3];
    for (unsigned v = 0; v < 3U; v++) {
        for (unsigned s = 0; s < 3U; s++) steps[v][s] = vld1q_u8(rows[v][s]);
    }
    static const int8_t lane_shift[16] = {6, 4, 2, 0, 6, 4, 2, 0, 6, 4, 2, 0, 6, 4, 2, 0};
    const int8x16_t shifts = vld1q_s8(lane_shift);

    uint32_t x = 0;
    for (; x + 48U <= width; x += 48U) {
        uint8x16_t packed[3];
        for (unsigned v = 0; v < 3U; v++) {
            uint8x16_t darkness = vmvnq_u8(vld1q_u8(gray + x + 16U * v));
            /* Each passed step is 0xFF; 0 - 0xFF - 0xFF ... counts them. */
            uint8x16_t level = vsubq_u8(vsubq_u8(vsubq_u8(vdupq_n_u8(0), vcgtq_u8(darkness, steps[v][0])),
                                                 vcgtq_u8(darkness, steps[v][1])),
                                        vcgtq_u8(darkness, steps[v][2]));
            packed[v] = vshlq_u8(level, shifts);
        }
        /* Two pairwise additions merge four 2-bit fields into one byte. */
        uint8x16_t bytes = vpaddq_u8(vpaddq_u8(packed[0], packed[1]), vpaddq_u8(packed[2], vdupq_n_u8(0)));
        uint8_t result[16];
        vst1q_u8(result, bytes);
        uint8_t *target = out + x / 4U;
        for (unsigned i = 0; i < 12U; i++) target[i] |= result[i];
    }
    return x;
}

static uint32_t screen_one_bit_neon(const uint8_t *gray, uint32_t width,
                                    uint32_t remaining, uint8_t *out) {
    const uint8x16_t thresholds = vld1q_u8(p1600_screen_1bpp + (remaining % 16U) * 16U);
    static const uint8_t bit_weights[16] = {128, 64, 32, 16, 8, 4, 2, 1, 128, 64, 32, 16, 8, 4, 2, 1};
    const uint8x16_t weights = vld1q_u8(bit_weights);

    uint32_t x = 0;
    for (; x + 16U <= width; x += 16U) {
        uint8x16_t darkness = vmvnq_u8(vld1q_u8(gray + x));
        uint8x16_t black = vandq_u8(vcgeq_u8(darkness, thresholds), vtstq_u8(darkness, darkness));
        uint8x16_t bits = vandq_u8(black, weights);
        out[x / 8U] |= vaddv_u8(vget_low_u8(bits));
        out[x / 8U + 1U] |= vaddv_u8(vget_high_u8(bits));
    }
    return x;
}
#endif

bool p1600_screen_simd_active(void) {
#if defined(__ARM_NEON)
    pthread_once(&thresholds_once, build_thresholds);
    return vector_screen;
#else
    return false;
#endif
}

void p1600_screen_line(const uint8_t *gray, uint32_t width, uint32_t bpp,
                    uint32_t remaining, uint8_t *out) {
    if (!gray || !out) return;
    uint32_t done = 0;
#if defined(__ARM_NEON)
    if (bpp == 2U) {
        if (p1600_screen_simd_active()) done = screen_two_bit_neon(gray, width, remaining, out);
    } else {
        done = screen_one_bit_neon(gray, width, remaining, out);
    }
#endif
    if (bpp == 2U) {
        screen_two_bit(gray, done, width, remaining, out);
    } else {
        screen_one_bit(gray, done, width, remaining, out);
    }
}
