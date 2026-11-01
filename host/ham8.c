/*
 * HAM8 for media.decode/1's VFRAME: base colours by median cut (or the
 * fixed 4x4x4 cube) and, pixel by pixel, the nearer of a base colour and
 * the pixel to the left with one channel changed.
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#include "ham8.h"

#include <stdlib.h>
#include <string.h>

static int dist(int r1, int g1, int b1, int r2, int g2, int b2)
{
    return 3 * (r1 - r2) * (r1 - r2) + 4 * (g1 - g2) * (g1 - g2) + 2 * (b1 - b2) * (b1 - b2);
}

/* Median cut over a 5-bit-a-channel histogram. */
struct colour {
    uint16_t c;                   /* 5:5:5 */
    uint32_t count;
};

static int axis;

static int by_axis(const void *a, const void *b)
{
    int s = axis * 5;
    int x = (((const struct colour *)a)->c >> s) & 31, y = (((const struct colour *)b)->c >> s) & 31;
    return x - y;
}

static int median_cut(const uint8_t *rgb, int pitch, uint32_t w, uint32_t h, uint8_t pal[HAM8_PALETTE])
{
    uint32_t *hist = calloc(32768, sizeof *hist), x, y;
    struct colour *col;
    int n = 0, i, boxes = 1, b, start[HAM8_BASE], end[HAM8_BASE];

    if (!hist)
        return 0;
    for (y = 0; y < h; y++) {
        const uint8_t *s = rgb + (size_t)y * pitch;
        for (x = 0; x < w; x++, s += 3)
            hist[(s[0] >> 3) << 10 | (s[1] >> 3) << 5 | s[2] >> 3]++;
    }
    for (i = 0; i < 32768; i++)
        n += hist[i] != 0;
    if (!(col = malloc((size_t)(n ? n : 1) * sizeof *col))) {
        free(hist);
        return 0;
    }
    for (i = n = 0; i < 32768; i++)
        if (hist[i]) {
            col[n].c = (uint16_t)i;
            col[n++].count = hist[i];
        }
    free(hist);
    start[0] = 0;
    end[0] = n;
    while (boxes < HAM8_BASE) {
        int best = -1, best_axis = 0, best_range = 0;
        for (b = 0; b < boxes; b++) {
            int lo[3] = { 31, 31, 31 }, hi[3] = { 0, 0, 0 }, k;
            if (end[b] - start[b] < 2)
                continue;
            for (i = start[b]; i < end[b]; i++)
                for (k = 0; k < 3; k++) {
                    int v = (col[i].c >> (k * 5)) & 31;
                    if (v < lo[k]) lo[k] = v;
                    if (v > hi[k]) hi[k] = v;
                }
            for (k = 0; k < 3; k++)
                if (hi[k] - lo[k] > best_range) {
                    best_range = hi[k] - lo[k];
                    best = b;
                    best_axis = k;
                }
        }
        if (best < 0)
            break;
        {
            uint64_t total = 0, run = 0;
            int mid;
            axis = best_axis;
            qsort(col + start[best], end[best] - start[best], sizeof col[0], by_axis);
            for (i = start[best]; i < end[best]; i++)
                total += col[i].count;
            for (mid = start[best]; mid < end[best] - 1; mid++) {
                run += col[mid].count;
                if (run * 2 >= total)
                    break;
            }
            if (mid == end[best] - 1)
                mid--;
            start[boxes] = mid + 1;
            end[boxes] = end[best];
            end[best] = mid + 1;
            boxes++;
        }
    }
    for (b = 0; b < boxes; b++) {
        uint64_t sum[3] = { 0, 0, 0 }, count = 0;
        int k;
        for (i = start[b]; i < end[b]; i++) {
            for (k = 0; k < 3; k++)
                sum[k] += (uint64_t)(((col[i].c >> (k * 5)) & 31) * 8 + 4) * col[i].count;
            count += col[i].count;
        }
        if (!count)
            count = 1;
        pal[b * 3] = (uint8_t)((sum[2] + count / 2) / count);       /* red is the top five bits */
        pal[b * 3 + 1] = (uint8_t)((sum[1] + count / 2) / count);
        pal[b * 3 + 2] = (uint8_t)((sum[0] + count / 2) / count);
    }
    free(col);
    return n ? boxes : 0;
}

void ham8_palette(const uint8_t *rgb, int pitch, uint32_t w, uint32_t h, int fixed, uint8_t pal[HAM8_PALETTE])
{
    int i, n = 0;

    if (!fixed)
        n = median_cut(rgb, pitch, w, h, pal);
    if (!n) {
        for (i = 0; i < HAM8_BASE; i++) {
            pal[i * 3] = (uint8_t)((i >> 4) * 85);
            pal[i * 3 + 1] = (uint8_t)(((i >> 2) & 3) * 85);
            pal[i * 3 + 2] = (uint8_t)((i & 3) * 85);
        }
        return;
    }
    for (i = n; i < HAM8_BASE; i++)                    /* a picture of few colours: the rest black */
        pal[i * 3] = pal[i * 3 + 1] = pal[i * 3 + 2] = 0;
}

void ham8_map(const uint8_t *rgb, int pitch, uint32_t w, uint32_t h, const uint8_t pal[HAM8_PALETTE], uint8_t *out)
{
    uint8_t lut[32768];
    uint32_t x, y;
    int i, k;

    /* The nearest base colour for each 5-bit colour. */
    for (i = 0; i < 32768; i++) {
        int r = (i >> 10) * 8 + 4, g = ((i >> 5) & 31) * 8 + 4, b = (i & 31) * 8 + 4, best = 0, bd = 1 << 30;
        for (k = 0; k < HAM8_BASE; k++) {
            int d = dist(r, g, b, pal[k * 3], pal[k * 3 + 1], pal[k * 3 + 2]);
            if (d < bd) {
                bd = d;
                best = k;
            }
        }
        lut[i] = (uint8_t)best;
    }
    for (y = 0; y < h; y++) {
        const uint8_t *s = rgb + (size_t)y * pitch;
        int pr = pal[0], pg = pal[1], pb = pal[2];
        for (x = 0; x < w; x++, s += 3) {
            int tr = s[0], tg = s[1], tb = s[2];
            int base = lut[(tr >> 3) << 10 | (tg >> 3) << 5 | tb >> 3];
            int nr = pal[base * 3], ng = pal[base * 3 + 1], nb = pal[base * 3 + 2];
            int bd = dist(tr, tg, tb, nr, ng, nb), best = base, d, v;

            v = (tr & 0xfc) | (pr & 3);                 /* modify red */
            if ((d = dist(tr, tg, tb, v, pg, pb)) < bd) {
                bd = d;
                best = 2 << 6 | tr >> 2;
                nr = v; ng = pg; nb = pb;
            }
            v = (tg & 0xfc) | (pg & 3);                 /* modify green */
            if ((d = dist(tr, tg, tb, pr, v, pb)) < bd) {
                bd = d;
                best = 3 << 6 | tg >> 2;
                nr = pr; ng = v; nb = pb;
            }
            v = (tb & 0xfc) | (pb & 3);                 /* modify blue */
            if ((d = dist(tr, tg, tb, pr, pg, v)) < bd) {
                best = 1 << 6 | tb >> 2;
                nr = pr; ng = pg; nb = v;
            }
            *out++ = (uint8_t)best;
            pr = nr;
            pg = ng;
            pb = nb;
        }
    }
}

void ham8_show(const uint8_t *ham, uint32_t w, uint32_t h, const uint8_t pal[HAM8_PALETTE], uint8_t *rgb)
{
    uint32_t x, y;

    for (y = 0; y < h; y++) {
        int r = pal[0], g = pal[1], b = pal[2];
        for (x = 0; x < w; x++) {
            int p = *ham++, v = p & 63;
            switch (p >> 6) {
            case 0: r = pal[v * 3]; g = pal[v * 3 + 1]; b = pal[v * 3 + 2]; break;
            case 1: b = v << 2 | (b & 3); break;
            case 2: r = v << 2 | (r & 3); break;
            default: g = v << 2 | (g & 3); break;
            }
            *rgb++ = (uint8_t)r;
            *rgb++ = (uint8_t)g;
            *rgb++ = (uint8_t)b;
        }
    }
}
