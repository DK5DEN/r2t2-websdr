/*
 * Check of the deep-zoom waterfall (wf_t.want): two carriers 30 Hz apart are one blob at
 * WF_FFT (47 Hz bins, the Blackman-Harris main lobe spans ~190 Hz) and two peaks with a
 * dip between them at WF_FFT_MAX (5.9 Hz bins, main lobe ~24 Hz).
 *   cc -O2 -Isrc tools/zoomtest.c src/dsp.c -lfftw3f -lm -o zoomtest && ./zoomtest
 */
#include <math.h>
#include <stdio.h>
#include "dsp.h"

static int run(int want, int *lines)
{
    static wf_t w;
    static uint8_t frame[WF_BINS], z[ZOOM_ROW];
    if (wf_init(&w, 5) < 0)
        return -1;
    w.want = want;
    float iq[2 * 128];
    double ph1 = 0, ph2 = 0, f1 = 1000.0, f2 = 1030.0;
    *lines = 0;
    for (int blk = 0; blk < FS_WIDE * 2 / 128; blk++) {   /* 2 s */
        for (int i = 0; i < 128; i++) {
            iq[2 * i] = (float)(0.1 * cos(ph1) + 0.1 * cos(ph2));
            iq[2 * i + 1] = (float)(0.1 * sin(ph1) + 0.1 * sin(ph2));
            ph1 += 2 * M_PI * f1 / FS_WIDE;
            ph2 += 2 * M_PI * f2 / FS_WIDE;
        }
        *lines += wf_push(&w, iq, 128, frame);
    }
    /* 200 Hz around the carriers, 2048 points: 0.1 Hz per point */
    wf_zoom(&w, 906.0, 200.0 / ZOOM_ROW, ZOOM_ROW, z);
    int a = z[(int)((1000 - 906) / (200.0 / ZOOM_ROW))], b = z[(int)((1030 - 906) / (200.0 / ZOOM_ROW))];
    int mid = z[(int)((1015 - 906) / (200.0 / ZOOM_ROW))];
    printf("FFT %5d: line bins %d, %d lines in 2 s, peaks %d/%d dBFS+170, between %d -> %s\n", want, w.nline,
           *lines, a, b, mid, (a - mid > 3 && b - mid > 3) ? "two carriers" : "one blob");
    return (a - mid > 3 && b - mid > 3) ? 2 : 1;
}

int main(void)
{
    if (dsp_global_init() < 0)
        return 1;
    int l1, l2;
    int coarse = run(WF_FFT, &l1), fine = run(WF_FFT_MAX, &l2);
    if (coarse != 1 || fine != 2 || l1 < 15 || l2 < 8) {
        printf("FAIL\n");
        return 1;
    }
    printf("OK\n");
    return 0;
}
