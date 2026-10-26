/* RGB565 -> ARGB8888 conversion and the optional edge-aware smoothing pass for
 * TizPSX. Header-only and dependency-free so tools/test_video_convert.c can
 * compare it with the original per-pixel implementation. Output is bit-exact
 * with that implementation; only the work done per pixel changed (luma is
 * computed once per pixel instead of five times, and neighbour channels are
 * only unpacked for the pixels that actually get smoothed).
 */
#ifndef TIZPSX_VIDEO_CONVERT_H
#define TIZPSX_VIDEO_CONVERT_H

#include <stdint.h>
#include <string.h>

static inline void psx_convert_rgb565(const uint16_t *pixels, unsigned pitch,
                                      unsigned width, unsigned height, uint32_t *dst)
{
    for (unsigned y = 0; y < height; ++y) {
        const uint16_t *src = pixels + y * pitch;
        uint32_t *out = dst + y * width;
        for (unsigned x = 0; x < width; ++x) {
            uint16_t p = src[x];
            unsigned r = (p >> 11) & 31u, g = (p >> 5) & 63u, b = p & 31u;
            out[x] = 0xff000000u |
                (r * 255u / 31u << 16) | (g * 255u / 63u << 8) | (b * 255u / 31u);
        }
    }
}

/* dst is smoothed in place. scratch holds w*h pixels, luma w*h bytes. */
static inline void psx_edge_smooth(uint32_t *dst, uint32_t *scratch, uint8_t *luma,
                                   unsigned w, unsigned h)
{
    const unsigned count = w * h;
    for (unsigned i = 0; i < count; ++i) {
        uint32_t c = dst[i];
        luma[i] = (uint8_t)((((c >> 16) & 255u) * 77u + ((c >> 8) & 255u) * 150u + (c & 255u) * 29u) >> 8);
    }
    memcpy(scratch, dst, (size_t)count * sizeof(uint32_t));
    for (unsigned y = 0; y < h; ++y) {
        const unsigned ym = y ? y - 1 : y, yp = y + 1 < h ? y + 1 : y;
        for (unsigned x = 0; x < w; ++x) {
            const unsigned xm = x ? x - 1 : x, xp = x + 1 < w ? x + 1 : x;
            const unsigned i = y * w + x;
            const unsigned in = ym * w + x, is = yp * w + x, ie = y * w + xp, iw = y * w + xm;
            int lc = luma[i];
            int lo = lc, hi = lc;
            int v = luma[in]; if (v < lo) lo = v; if (v > hi) hi = v;
            v = luma[is];     if (v < lo) lo = v; if (v > hi) hi = v;
            v = luma[ie];     if (v < lo) lo = v; if (v > hi) hi = v;
            v = luma[iw];     if (v < lo) lo = v; if (v > hi) hi = v;
            int contrast = hi - lo;
            /* Conservative edge-aware pass: smooth strong local luma
             * transitions while retaining at least 75% of the source. */
            unsigned mix = contrast > 72 ? 64u : contrast > 40 ? 48u : 0u;
            if (!mix) continue;                       /* scratch already holds the source pixel */
            uint32_t c = dst[i], n = dst[in], s = dst[is], e = dst[ie], west = dst[iw];
            int cr = (int)((c >> 16) & 255u), cg = (int)((c >> 8) & 255u), cb = (int)(c & 255u);
            int nr = (int)((n >> 16) & 255u), ng = (int)((n >> 8) & 255u), nb = (int)(n & 255u);
            int sr = (int)((s >> 16) & 255u), sg = (int)((s >> 8) & 255u), sb = (int)(s & 255u);
            int er = (int)((e >> 16) & 255u), eg = (int)((e >> 8) & 255u), eb = (int)(e & 255u);
            int wr = (int)((west >> 16) & 255u), wg = (int)((west >> 8) & 255u), wb = (int)(west & 255u);
            int rr = (cr * (256 - (int)mix) + (nr + sr + er + wr) * ((int)mix / 4)) >> 8;
            int gg = (cg * (256 - (int)mix) + (ng + sg + eg + wg) * ((int)mix / 4)) >> 8;
            int bb = (cb * (256 - (int)mix) + (nb + sb + eb + wb) * ((int)mix / 4)) >> 8;
            scratch[i] = 0xff000000u | (unsigned)rr << 16 | (unsigned)gg << 8 | (unsigned)bb;
        }
    }
    memcpy(dst, scratch, (size_t)count * sizeof(uint32_t));
}

#endif
