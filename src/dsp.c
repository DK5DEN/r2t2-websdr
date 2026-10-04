#include <math.h>
#include <string.h>

#include "dsp.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define AGC_TARGET  0.3f
#define AGC_HANG    (FS_NARROW / 2)   /* 0.5 s */
#define AGC_DECAY   0.99995f          /* ~1.25 s time constant */
#define AGC_FLOOR   1e-6f

static fftwf_plan wf_plan;
static void ddc_global_init(void);
static float wf_win[WF_FFT];
static float wf_norm;

int dsp_global_init(void)
{
    fftwf_complex *a = fftwf_malloc(sizeof(fftwf_complex) * WF_FFT);
    fftwf_complex *b = fftwf_malloc(sizeof(fftwf_complex) * WF_FFT);
    if (!a || !b)
        return -1;
    wf_plan = fftwf_plan_dft_1d(WF_FFT, a, b, FFTW_FORWARD, FFTW_MEASURE);
    fftwf_free(a);
    fftwf_free(b);
    if (!wf_plan)
        return -1;

    ddc_global_init();

    /* 4-term Blackman-Harris */
    double sum = 0;
    for (int i = 0; i < WF_FFT; i++) {
        double x = 2 * M_PI * i / (WF_FFT - 1);
        double w = 0.35875 - 0.48829 * cos(x) + 0.14128 * cos(2 * x) - 0.01168 * cos(3 * x);
        wf_win[i] = (float)w;
        sum += w;
    }
    /* full-scale complex sine -> 0 dBFS */
    wf_norm = (float)(1.0 / (sum * sum));
    return 0;
}

/* ---------------------------------------------------------------- waterfall */

int wf_init(wf_t *w, int navg)
{
    w->in = fftwf_malloc(sizeof(fftwf_complex) * WF_FFT);
    w->out = fftwf_malloc(sizeof(fftwf_complex) * WF_FFT);
    /* one line every navg FFT lengths, of which WF_AVG are computed:
       with 8 receivers on waterfalls the FFTs were most of the CPU load */
    w->period = (navg < 1 ? 1 : navg) * WF_FFT;
    w->navg = navg < WF_AVG ? (navg < 1 ? 1 : navg) : WF_AVG;
    wf_reset(w);
    return (w->in && w->out) ? 0 : -1;
}

void wf_reset(wf_t *w)
{
    w->fill = 0;
    w->nacc = 0;
    w->pos = 0;
    memset(w->acc, 0, sizeof(w->acc));
}

/* One byte per bin: dBFS + 170, max-pooled from WF_FFT to WF_BINS, DC in the middle. */
static void wf_frame(wf_t *w, uint8_t *frame)
{
    const int r = WF_FFT / WF_BINS;
    const float scale = wf_norm / w->navg;
    for (int b = 0; b < WF_BINS; b++) {
        float m = 0;
        for (int j = 0; j < r; j++) {
            int src = (b * r + j + WF_FFT / 2) & (WF_FFT - 1);
            if (w->acc[src] > m)
                m = w->acc[src];
        }
        int v = (int)lrintf(10.0f * log10f(m * scale + 1e-20f) + 170.0f);
        frame[b] = v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
    }
}

int wf_push(wf_t *w, const float *iq, int n, uint8_t *frame)
{
    int ready = 0;
    for (int i = 0; i < n; i++) {
        int skip = w->pos >= w->navg * WF_FFT;
        if (++w->pos >= w->period)
            w->pos = 0;
        if (skip)
            continue;
        int f = w->fill;
        w->in[f][0] = iq[2 * i] * wf_win[f];
        w->in[f][1] = iq[2 * i + 1] * wf_win[f];
        if (++w->fill < WF_FFT)
            continue;

        w->fill = 0;
        fftwf_execute_dft(wf_plan, w->in, w->out);
        for (int k = 0; k < WF_FFT; k++)
            w->acc[k] += w->out[k][0] * w->out[k][0] + w->out[k][1] * w->out[k][1];
        if (++w->nacc >= w->navg) {
            wf_frame(w, frame);
            ready = 1;
            w->nacc = 0;
            memset(w->acc, 0, sizeof(w->acc));
        }
    }
    return ready;
}

/* ---------------------------------------------------------------- IMA ADPCM */

static const int ima_index_tab[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
static const int ima_step_tab[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767
};

static uint8_t ima_encode(ima_t *s, int16_t x)
{
    int step = ima_step_tab[s->index];
    int diff = x - s->pred;
    int code = 0;
    if (diff < 0) {
        code = 8;
        diff = -diff;
    }
    int vpdiff = step >> 3;
    if (diff >= step) { code |= 4; diff -= step; vpdiff += step; }
    step >>= 1;
    if (diff >= step) { code |= 2; diff -= step; vpdiff += step; }
    step >>= 1;
    if (diff >= step) { code |= 1; vpdiff += step; }

    int pred = s->pred + ((code & 8) ? -vpdiff : vpdiff);
    s->pred = (int16_t)(pred > 32767 ? 32767 : pred < -32768 ? -32768 : pred);
    s->index += ima_index_tab[code];
    if (s->index < 0) s->index = 0;
    if (s->index > 88) s->index = 88;
    return (uint8_t)code;
}

/* ---------------------------------------------------------------- wide-stream listener */

/*
 * Measured on the R2T2 (Cortex-A9, NEON): ~4 % of a core per listener with
 * 160 taps (192 used here) and one decimation stage; two stages (/4, /3) were not cheaper.
 * Taps are shared, the dot products run on separate I/Q arrays so the
 * compiler vectorises them.
 */
static float ddc_taps[DDC_NT];

static void ddc_global_init(void)
{
    /* -6 dB at 8 kHz (output Nyquist), Blackman transition ~5.3..10.7 kHz:
       SSB/CW flat, AM/FM edges slightly damped; what folds back into the
       SSB passband (16..19 kHz) is fully in the stop band */
    double h[DDC_NT], sum = 0;
    for (int n = 0; n < DDC_NT; n++) {
        double m = n - (DDC_NT - 1) / 2.0;
        double fc = 8000.0 / FS_WIDE;
        double s = m == 0 ? 2 * fc : sin(2 * M_PI * fc * m) / (M_PI * m);
        h[n] = s * (0.42 - 0.5 * cos(2 * M_PI * n / (DDC_NT - 1)) + 0.08 * cos(4 * M_PI * n / (DDC_NT - 1)));
        sum += h[n];
    }
    for (int n = 0; n < DDC_NT; n++)
        ddc_taps[n] = (float)(h[n] / sum);
}

void ddc_init(ddc_t *d)
{
    memset(d, 0, sizeof(*d));
    d->pr = 1;
    d->dr = 1;
}

void ddc_set(ddc_t *d, double offset_hz)
{
    /* multiply by exp(-j 2 pi offset t): the listener frequency lands on 0 Hz */
    d->dr = (float)cos(-2 * M_PI * offset_hz / FS_WIDE);
    d->di = (float)sin(-2 * M_PI * offset_hz / FS_WIDE);
}

static float dot(const float *a, const float *h)
{
    float s = 0;
    for (int k = 0; k < DDC_NT; k++)
        s += a[k] * h[k];
    return s;
}

/* iq: n complex samples at FS_WIDE; out: up to max complex samples at FS_NARROW */
int ddc_process(ddc_t *d, const float *iq, int n, float *out, int max)
{
    int m = 0;
    float pr = d->pr, pi = d->pi;
    for (int i = 0; i < n; i++) {
        if (d->fill == DDC_BUF) {
            memmove(d->xr, d->xr + DDC_BUF - DDC_NT, DDC_NT * sizeof(float));
            memmove(d->xi, d->xi + DDC_BUF - DDC_NT, DDC_NT * sizeof(float));
            d->fill = DDC_NT;
        }
        float a = iq[2 * i], b = iq[2 * i + 1];
        d->xr[d->fill] = a * pr - b * pi;
        d->xi[d->fill] = a * pi + b * pr;
        d->fill++;
        float t = pr * d->dr - pi * d->di;
        pi = pr * d->di + pi * d->dr;
        pr = t;
        if (++d->phase >= DDC_DEC && d->fill >= DDC_NT) {
            d->phase = 0;
            if (m < max) {
                out[2 * m] = dot(d->xr + d->fill - DDC_NT, ddc_taps);
                out[2 * m + 1] = dot(d->xi + d->fill - DDC_NT, ddc_taps);
                m++;
            }
        }
    }
    float g = 1.0f / sqrtf(pr * pr + pi * pi);
    d->pr = pr * g;
    d->pi = pi * g;
    return m;
}

/* ---------------------------------------------------------------- demodulator */

static double blackman(int n, int N)
{
    return 0.42 - 0.5 * cos(2 * M_PI * n / (N - 1)) + 0.08 * cos(4 * M_PI * n / (N - 1));
}

/* windowed-sinc lowpass, cutoff in cycles/sample, unity DC gain */
static void lowpass(double *h, int N, double fc)
{
    int M = N / 2;
    double sum = 0;
    for (int n = 0; n < N; n++) {
        int m = n - M;
        double s = m == 0 ? 2 * fc : sin(2 * M_PI * fc * m) / (M_PI * m);
        h[n] = s * blackman(n, N);
        sum += h[n];
    }
    for (int n = 0; n < N; n++)
        h[n] /= sum;
}

void demod_init(demod_t *d)
{
    memset(d, 0, sizeof(*d));
    double h[DM_NA];
    lowpass(h, DM_NA, 3600.0 / FS_NARROW);
    for (int n = 0; n < DM_NA; n++)
        d->ataps[n] = (float)h[DM_NA - 1 - n];
    d->peak = 1e-4f;
    d->sql = -999;
    demod_set(d, M_USB, 300, 2700);
}

void demod_set(demod_t *d, int mode, float lo, float hi)
{
    double h[DM_NT];
    double fc = (hi - lo) / 2.0 / FS_NARROW;
    double f0 = (hi + lo) / 2.0 / FS_NARROW;
    int M = DM_NT / 2;

    lowpass(h, DM_NT, fc);
    /* shift the lowpass to the passband centre; store reversed for the dot product */
    for (int n = 0; n < DM_NT; n++) {
        int m = n - M;
        d->tr[DM_NT - 1 - n] = (float)(h[n] * cos(2 * M_PI * f0 * m));
        d->ti[DM_NT - 1 - n] = (float)(h[n] * sin(2 * M_PI * f0 * m));
    }
    if (mode != d->mode) {
        d->dc = 0;
        d->peak = 1e-4f;
        d->hang = 0;
    }
    d->mode = mode;
    d->lo = lo;
    d->hi = hi;
}

float demod_level_db(const demod_t *d)
{
    return 10.0f * log10f(d->pwr + 1e-20f);
}

void demod_process(demod_t *d, const float *iq, int n)
{
    for (int i = 0; i < n; i++) {
        /* complex channel filter */
        d->xr[d->hp] = d->xr[d->hp + DM_NT] = iq[2 * i];
        d->xi[d->hp] = d->xi[d->hp + DM_NT] = iq[2 * i + 1];
        const float *br = &d->xr[d->hp + 1], *bi = &d->xi[d->hp + 1];
        float yr = 0, yi = 0;
        for (int k = 0; k < DM_NT; k++) {
            yr += d->tr[k] * br[k] - d->ti[k] * bi[k];
            yi += d->tr[k] * bi[k] + d->ti[k] * br[k];
        }
        if (++d->hp == DM_NT)
            d->hp = 0;

        float p = yr * yr + yi * yi;
        d->pwr += (p - d->pwr) * 0.002f;

        float a;
        switch (d->mode) {
        case M_AM: {
            float m = sqrtf(p);
            d->dc += (m - d->dc) * 0.0005f;
            a = m - d->dc;
            break;
        }
        case M_FM: {
            float zr = yr * d->pr + yi * d->pi;
            float zi = yi * d->pr - yr * d->pi;
            a = atan2f(zi, zr) * (float)(1.5 / M_PI);
            d->pr = yr;
            d->pi = yi;
            break;
        }
        default:
            a = yr;
            break;
        }

        if (d->mode != M_FM) {
            float m = fabsf(a);
            if (m > d->peak) {
                d->peak = m;
                d->hang = AGC_HANG;
            } else if (d->hang > 0) {
                d->hang--;
            } else {
                d->peak *= AGC_DECAY;
            }
            if (d->peak < AGC_FLOOR)
                d->peak = AGC_FLOOR;
            a *= AGC_TARGET / d->peak;
        }

        if (d->sql > -200) {
            float db = demod_level_db(d);
            if (d->sqopen && db < d->sql - 3)
                d->sqopen = 0;
            else if (!d->sqopen && db > d->sql)
                d->sqopen = 1;
            if (!d->sqopen)
                a = 0;
        }

        /* anti-alias lowpass, keep every second sample */
        d->ah[d->ap] = d->ah[d->ap + DM_NA] = a;
        const float *ab = &d->ah[d->ap + 1];
        if (++d->ap == DM_NA)
            d->ap = 0;
        d->dec ^= 1;
        if (!d->dec)
            continue;
        float s = 0;
        for (int k = 0; k < DM_NA; k++)
            s += d->ataps[k] * ab[k];
        if (s > 1) s = 1;
        if (s < -1) s = -1;
        if (d->outn < DM_OUTMAX)
            d->out[d->outn++] = (int16_t)(s * 32767.0f);
    }
}

/*
 * Audio packet: [0x02][int16 level dBFS*10][int16 predictor][uint8 index][AUDIO_BLOCK/2 bytes]
 * The decoder state travels with every packet, so dropped packets do not desync the browser.
 */
int demod_packet(demod_t *d, uint8_t *pkt)
{
    if (d->outn < AUDIO_BLOCK)
        return 0;

    int lvl = (int)lrintf(demod_level_db(d) * 10.0f);
    if (lvl < -32768) lvl = -32768;
    if (lvl > 32767) lvl = 32767;
    pkt[0] = 2;
    pkt[1] = lvl & 0xff;
    pkt[2] = (lvl >> 8) & 0xff;
    pkt[3] = d->ima.pred & 0xff;
    pkt[4] = (d->ima.pred >> 8) & 0xff;
    pkt[5] = (uint8_t)d->ima.index;
    for (int i = 0; i < AUDIO_BLOCK / 2; i++) {
        uint8_t lo = ima_encode(&d->ima, d->out[2 * i]);
        uint8_t hi = ima_encode(&d->ima, d->out[2 * i + 1]);
        pkt[6 + i] = lo | (hi << 4);
    }
    d->outn -= AUDIO_BLOCK;
    memmove(d->out, d->out + AUDIO_BLOCK, d->outn * sizeof(d->out[0]));
    return 6 + AUDIO_BLOCK / 2;
}
