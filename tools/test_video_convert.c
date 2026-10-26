/* Host test: cc -O2 -std=gnu11 -I app/ui tools/test_video_convert.c -o /tmp/t && /tmp/t
 * Compares ui/video_convert.h with the original per-pixel implementation
 * copied verbatim from psx_main.c 0.2.12 (publish_video), then times both. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "video_convert.h"

static void reference(const uint16_t *pixels, unsigned pitch, unsigned width, unsigned height,
                      int antialiasing, uint32_t *dst, uint32_t *aa_scratch)
{
    for (unsigned y = 0; y < height; ++y) {
        const uint16_t *src = pixels + y * pitch;
        for (unsigned x = 0; x < width; ++x) {
            uint16_t p = src[x];
            unsigned r = (p >> 11) & 31u, g = (p >> 5) & 63u, b = p & 31u;
            dst[y * width + x] = 0xff000000u |
                (r * 255u / 31u << 16) | (g * 255u / 63u << 8) | (b * 255u / 31u);
        }
    }
    if (antialiasing && width >= 3 && height >= 3) {
        const unsigned w = width, h = height;
        for (unsigned y = 0; y < h; ++y) {
            unsigned ym = y ? y - 1 : y, yp = y + 1 < h ? y + 1 : y;
            for (unsigned x = 0; x < w; ++x) {
                unsigned xm = x ? x - 1 : x, xp = x + 1 < w ? x + 1 : x;
                unsigned i = y * w + x;
                uint32_t c = dst[i];
                uint32_t n = dst[ym * w + x], s = dst[yp * w + x];
                uint32_t e = dst[y * w + xp], west = dst[y * w + xm];
                int cr = (int)((c >> 16) & 255u), cg = (int)((c >> 8) & 255u), cb = (int)(c & 255u);
                int nr = (int)((n >> 16) & 255u), ng = (int)((n >> 8) & 255u), nb = (int)(n & 255u);
                int sr = (int)((s >> 16) & 255u), sg = (int)((s >> 8) & 255u), sb = (int)(s & 255u);
                int er = (int)((e >> 16) & 255u), eg = (int)((e >> 8) & 255u), eb = (int)(e & 255u);
                int wr = (int)((west >> 16) & 255u), wg = (int)((west >> 8) & 255u), wb = (int)(west & 255u);
                int ln = (nr * 77 + ng * 150 + nb * 29) >> 8;
                int ls = (sr * 77 + sg * 150 + sb * 29) >> 8;
                int le = (er * 77 + eg * 150 + eb * 29) >> 8;
                int lw = (wr * 77 + wg * 150 + wb * 29) >> 8;
                int lc = (cr * 77 + cg * 150 + cb * 29) >> 8;
                int lo = lc;
                int hi = lc;
                if (ln < lo) lo = ln;
                if (ln > hi) hi = ln;
                if (ls < lo) lo = ls;
                if (ls > hi) hi = ls;
                if (le < lo) lo = le;
                if (lw < lo) lo = lw;
                if (le > hi) hi = le;
                if (lw > hi) hi = lw;
                int contrast = hi - lo;
                unsigned mix = contrast > 72 ? 64u : contrast > 40 ? 48u : 0u;
                if (mix) {
                    int rr = (cr * (256 - (int)mix) + (nr + sr + er + wr) * ((int)mix / 4)) >> 8;
                    int gg = (cg * (256 - (int)mix) + (ng + sg + eg + wg) * ((int)mix / 4)) >> 8;
                    int bb = (cb * (256 - (int)mix) + (nb + sb + eb + wb) * ((int)mix / 4)) >> 8;
                    aa_scratch[i] = 0xff000000u | (unsigned)rr << 16 | (unsigned)gg << 8 | (unsigned)bb;
                } else aa_scratch[i] = c;
            }
        }
        memcpy(dst, aa_scratch, (size_t)w * h * sizeof(uint32_t));
    }
}

static void fill(uint16_t *px, unsigned pitch, unsigned w, unsigned h, int kind)
{
    for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
        uint16_t v;
        switch (kind) {
        case 0: v = (uint16_t)rand(); break;                                   /* noise */
        case 1: v = (uint16_t)((x * 31 / w) << 11 | (y * 63 / h) << 5 | ((x ^ y) & 31)); break; /* gradients */
        case 2: v = ((x / 8 + y / 8) & 1) ? 0xffff : 0x0000; break;            /* hard checker edges */
        default: v = (uint16_t)((x * 7 + y * 13) % 5 ? 0x4208 : 0xf800); break; /* mostly flat + sparse */
        }
        px[y * pitch + x] = v;
    }
}

int main(void)
{
    static const unsigned sizes[][2] = { {3,3}, {4,3}, {3,9}, {5,7}, {64,48}, {320,240}, {368,240}, {640,480}, {1024,512} };
    srand(12345);
    for (unsigned s = 0; s < sizeof sizes / sizeof sizes[0]; ++s) {
        unsigned w = sizes[s][0], h = sizes[s][1], pitch = w + (s & 1 ? 0 : 7);
        uint16_t *px = malloc((size_t)pitch * h * 2);
        uint32_t *a = malloc((size_t)w * h * 4), *b = malloc((size_t)w * h * 4), *scr = malloc((size_t)w * h * 4);
        uint8_t *luma = malloc((size_t)w * h);
        for (int kind = 0; kind < 4; ++kind) for (int aa = 0; aa < 2; ++aa) {
            fill(px, pitch, w, h, kind);
            reference(px, pitch, w, h, aa, a, scr);
            psx_convert_rgb565(px, pitch, w, h, b);
            if (aa && w >= 3 && h >= 3) psx_edge_smooth(b, scr, luma, w, h);
            assert(memcmp(a, b, (size_t)w * h * 4) == 0);
        }
        free(px); free(a); free(b); free(scr); free(luma);
    }
    printf("bit-exact with the original on %zu sizes x 4 contents x AA on/off\n", sizeof sizes / sizeof sizes[0]);

    /* timing, 2x resolution frame: 640x480 */
    unsigned w = 640, h = 480;
    uint16_t *px = malloc((size_t)w * h * 2);
    uint32_t *a = malloc((size_t)w * h * 4), *scr = malloc((size_t)w * h * 4);
    uint8_t *luma = malloc((size_t)w * h);
    fill(px, w, w, h, 1);
    struct timespec t0, t1; double ref_ms, new_ms;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 50; ++i) reference(px, w, w, h, 1, a, scr);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    ref_ms = ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / 50;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 50; ++i) { psx_convert_rgb565(px, w, w, h, a); psx_edge_smooth(a, scr, luma, w, h); }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    new_ms = ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / 50;
    printf("host x86-64 640x480 gradients: original %.2f ms/frame, new %.2f ms/frame (%.1fx)\n", ref_ms, new_ms, ref_ms / new_ms);
    free(px); free(a); free(scr); free(luma);
    return 0;
}
