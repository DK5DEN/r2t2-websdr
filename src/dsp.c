#include <math.h>
#include <stdlib.h>
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
/* larger waterfall FFTs for deep zoom, made on first use: index log2(n / WF_FFT) */
#define WF_NBIG 4
static fftwf_plan wf_big_plan[WF_NBIG];
static float *wf_big_win[WF_NBIG];
static float wf_big_norm[WF_NBIG];
static fftwf_plan fc_plan_f, fc_plan_i, cf_plan_f, cf_plan_i;   /* FFTW_UNALIGNED: any buffer */
static void fc_global_init(void);
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

    {
        fftwf_complex *x = fftwf_malloc(sizeof(fftwf_complex) * FC_NF);
        fftwf_complex *y = fftwf_malloc(sizeof(fftwf_complex) * FC_NF);
        unsigned fl = FFTW_MEASURE | FFTW_UNALIGNED;
        fc_plan_f = fftwf_plan_dft_1d(FC_NF, x, y, FFTW_FORWARD, fl);
        fc_plan_i = fftwf_plan_dft_1d(FC_NI, x, x, FFTW_BACKWARD, fl);
        cf_plan_f = fftwf_plan_dft_1d(CF_N, x, y, FFTW_FORWARD, fl);
        cf_plan_i = fftwf_plan_dft_1d(CF_N, x, y, FFTW_BACKWARD, fl);
        fftwf_free(x);
        fftwf_free(y);
        if (!fc_plan_f || !fc_plan_i || !cf_plan_f || !cf_plan_i)
            return -1;
        fc_global_init();
    }

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

static int big_index(int n)
{
    int i = 0;
    while ((WF_FFT << i) < n && i < WF_NBIG - 1)
        i++;
    return i;
}

/* plan and window of an n-point FFT (WF_FFT < n <= WF_FFT_MAX); 0 if out of memory */
static int big_ready(int n, fftwf_complex *in, fftwf_complex *out)
{
    int i = big_index(n);
    if (wf_big_plan[i])
        return 1;
    float *win = malloc(sizeof(float) * n);
    if (!win)
        return 0;
    /* FFTW_ESTIMATE: measuring a 32768-point plan would stall the stream for seconds */
    wf_big_plan[i] = fftwf_plan_dft_1d(n, in, out, FFTW_FORWARD, FFTW_ESTIMATE | FFTW_UNALIGNED);
    if (!wf_big_plan[i]) {
        free(win);
        return 0;
    }
    double sum = 0;
    for (int k = 0; k < n; k++) {
        double x = 2 * M_PI * k / (n - 1);
        win[k] = (float)(0.35875 - 0.48829 * cos(x) + 0.14128 * cos(2 * x) - 0.01168 * cos(3 * x));
        sum += win[k];
    }
    wf_big_win[i] = win;
    wf_big_norm[i] = (float)(1.0 / (sum * sum));
    return 1;
}

int wf_init(wf_t *w, int navg)
{
    w->in = fftwf_malloc(sizeof(fftwf_complex) * WF_FFT);
    w->out = fftwf_malloc(sizeof(fftwf_complex) * WF_FFT);
    /* one line every navg FFT lengths, of which WF_AVG are computed:
       with 8 receivers on waterfalls the FFTs were most of the CPU load */
    w->base_period = (navg < 1 ? 1 : navg) * WF_FFT;
    w->base_navg = navg < WF_AVG ? (navg < 1 ? 1 : navg) : WF_AVG;
    w->period = w->base_period;
    w->navg = w->base_navg;
    w->n = w->want = WF_FFT;
    w->bin = w->bout = NULL;
    w->bacc = w->bspec = NULL;
    w->line = w->spec;
    w->nline = WF_FFT;
    wf_reset(w);
    return (w->in && w->out) ? 0 : -1;
}

/* at the start of a line period: take the FFT size the main loop asked for */
static void wf_size(wf_t *w)
{
    int n = w->want;
    if (n < WF_FFT || n > WF_FFT_MAX || (n & (n - 1)))
        n = WF_FFT;
    if (n > WF_FFT && !w->bin) {
        w->bin = fftwf_malloc(sizeof(fftwf_complex) * WF_FFT_MAX);
        w->bout = fftwf_malloc(sizeof(fftwf_complex) * WF_FFT_MAX);
        w->bacc = calloc(WF_FFT_MAX, sizeof(float));
        w->bspec = calloc(WF_FFT_MAX, sizeof(float));
        if (!w->bin || !w->bout || !w->bacc || !w->bspec) {
            fftwf_free(w->bin);
            fftwf_free(w->bout);
            free(w->bacc);
            free(w->bspec);
            w->bin = w->bout = NULL;
            w->bacc = w->bspec = NULL;
        }
    }
    if (n > WF_FFT && (!w->bin || !big_ready(n, w->bin, w->bout)))
        n = WF_FFT;
    if (n == w->n)
        return;
    w->n = n;
    w->navg = n == WF_FFT ? w->base_navg : 1;
    w->period = n == WF_FFT ? w->base_period : (n > w->base_period ? n : w->base_period);
}

void wf_reset(wf_t *w)
{
    w->fill = 0;
    w->nacc = 0;
    w->pos = 0;
    memset(w->acc, 0, sizeof(w->acc));
    if (w->bacc)
        memset(w->bacc, 0, sizeof(float) * WF_FFT_MAX);
}

static uint8_t wf_byte(float p)
{
    int v = (int)lrintf(10.0f * log10f(p + 1e-20f) + 170.0f);
    return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

/*
 * The next n samples fall into the unused part of the line period: advance without them
 * and return 1, so the caller need not even unpack them. Returns 0 (nothing advanced) if
 * any of them would be used.
 */
int wf_skip(wf_t *w, int n)
{
    if (w->pos < w->navg * w->n || w->pos + n > w->period)
        return 0;
    w->pos += n;
    if (w->pos >= w->period)
        w->pos = 0;
    return 1;
}

/* One byte per bin: dBFS + 170, max-pooled from WF_FFT to WF_BINS, DC in the middle.
   The full-resolution line stays in spec for zoomed viewers. */
static void wf_frame(wf_t *w, uint8_t *frame)
{
    const int n = w->n, r = n / WF_BINS;
    const int big = n > WF_FFT;
    float *acc = big ? w->bacc : w->acc, *spec = big ? w->bspec : w->spec;
    const float scale = (big ? wf_big_norm[big_index(n)] : wf_norm) / w->navg;
    for (int k = 0; k < n; k++)
        spec[k] = acc[(k + n / 2) & (n - 1)] * scale;
    w->line = spec;
    w->nline = n;
    for (int b = 0; b < WF_BINS; b++) {
        float m = 0;
        for (int j = 0; j < r; j++)
            if (spec[b * r + j] > m)
                m = spec[b * r + j];
        frame[b] = wf_byte(m);
    }
}

/*
 * Zoomed line: n points df Hz apart, the first at rel0 Hz from the receiver
 * centre; each point is the maximum of the FFT bins it covers (or the one bin
 * it falls in when zoomed in beyond the FFT resolution of ~47 Hz).
 */
void wf_zoom(const wf_t *w, double rel0, double df, int n, uint8_t *out)
{
    const int nb = w->nline;
    const float *spec = w->line;
    const double bpf = (double)nb / FS_WIDE;   /* bins per Hz */
    for (int i = 0; i < n; i++) {
        double c = (rel0 + i * df) * bpf + nb / 2;
        int b0 = (int)floor(c - df * bpf / 2), b1 = (int)floor(c + df * bpf / 2);
        if (b1 < b0)
            b1 = b0;
        if (b0 < 0) b0 = 0;
        if (b1 > nb - 1) b1 = nb - 1;
        if (b0 > nb - 1) b0 = nb - 1;
        float m = 0;
        for (int k = b0; k <= b1; k++)
            if (spec[k] > m)
                m = spec[k];
        out[i] = wf_byte(m);
    }
}

int wf_push(wf_t *w, const float *iq, int n, uint8_t *frame)
{
    int ready = 0;
    for (int i = 0; i < n; i++) {
        if (w->pos == 0 && w->fill == 0 && w->nacc == 0 && w->want != w->n)
            wf_size(w);
        const int big = w->n > WF_FFT, nn = w->n;
        fftwf_complex *in = big ? w->bin : w->in, *out = big ? w->bout : w->out;
        const float *win = big ? wf_big_win[big_index(nn)] : wf_win;
        float *acc = big ? w->bacc : w->acc;
        int skip = w->pos >= w->navg * nn;
        if (++w->pos >= w->period)
            w->pos = 0;
        if (skip)
            continue;
        int f = w->fill;
        in[f][0] = iq[2 * i] * win[f];
        in[f][1] = iq[2 * i + 1] * win[f];
        if (++w->fill < nn)
            continue;

        w->fill = 0;
        fftwf_execute_dft(big ? wf_big_plan[big_index(nn)] : wf_plan, in, out);
        for (int k = 0; k < nn; k++)
            acc[k] += out[k][0] * out[k][0] + out[k][1] * out[k][1];
        if (++w->nacc >= w->navg) {
            wf_frame(w, frame);
            ready = 1;
            w->nacc = 0;
            memset(acc, 0, sizeof(float) * nn);
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
 * Measured on the R2T2: the segment FFT costs ~5.7 % of a core, each listener's part
 * (512 bins, inverse FFT) ~0.3 %; the former per-listener mixer and 192-tap decimating
 * filter cost ~5 %.
 */
static float fc_H[2 * FC_NF];   /* anti-alias low-pass spectrum, scaled by 1/FC_NF */

static void fc_global_init(void)
{
    /* -6 dB at 7.5 kHz, Blackman skirts ~0.7 kHz: flat to ~7.1 kHz, stop from ~7.9 kHz */
    static float h[2 * FC_NF];
    memset(h, 0, sizeof(h));
    double sum = 0, fc = 7500.0 / FS_WIDE;
    for (int n = 0; n < FC_LP; n++) {
        double m = n - (FC_LP - 1) / 2.0;
        double v = m == 0 ? 2 * fc : sin(2 * M_PI * fc * m) / (M_PI * m);
        v *= 0.42 - 0.5 * cos(2 * M_PI * n / (FC_LP - 1)) + 0.08 * cos(4 * M_PI * n / (FC_LP - 1));
        h[2 * n] = (float)v;
        sum += v;
    }
    for (int n = 0; n < FC_LP; n++)
        h[2 * n] = (float)(h[2 * n] / sum);
    fftwf_execute_dft(fc_plan_f, (fftwf_complex *)h, (fftwf_complex *)fc_H);
    for (int k = 0; k < 2 * FC_NF; k++)
        fc_H[k] /= FC_NF;
}

void fc_seg_reset(fcseg_t *s)
{
    memset(s->x, 0, sizeof(s->x));
    s->fill = FC_NF - FC_HOP;
    s->ready = 0;
    s->m = 0;
}

/* take samples until a window is complete; returns how many were taken */
int fc_seg_feed(fcseg_t *s, const float *iq, int n)
{
    int k = FC_NF - s->fill;
    if (k > n)
        k = n;
    memcpy(s->x + 2 * s->fill, iq, 2 * k * sizeof(float));
    s->fill += k;
    if (s->fill == FC_NF) {
        fftwf_execute_dft(fc_plan_f, (fftwf_complex *)s->x, (fftwf_complex *)s->X);
        memmove(s->x, s->x + 2 * FC_HOP, 2 * (FC_NF - FC_HOP) * sizeof(float));
        s->fill = FC_NF - FC_HOP;
        s->ready = 1;
        s->m++;
    }
    return k;
}

void fc_set(fclis_t *l, double offset_hz)
{
    const double bin = (double)FS_WIDE / FC_NF;   /* 31.25 Hz */
    l->kc = (int)lround(offset_hz / bin);
    double res = offset_hz - l->kc * bin;
    l->sr = (float)cos(-2 * M_PI * res / FS_NARROW);
    l->si = (float)sin(-2 * M_PI * res / FS_NARROW);
    if (l->rr == 0 && l->ri == 0)
        l->rr = 1;
}

/* FC_OUT samples at 16 kS/s of the last window, shifted by the listener offset */
int fc_out(const fcseg_t *s, fclis_t *l, float *out)
{
    /* output bin j is input bin kc + j (j < FC_NI/2) or kc + j - FC_NI, low-pass bin j or
       FC_NF + j - FC_NI; both walk forward with one wrap each */
    int src = ((l->kc % FC_NF) + FC_NF) % FC_NF, hh = 0;
    for (int j = 0; j < FC_NI; j++) {
        if (j == FC_NI / 2) {
            src = (((l->kc - FC_NI / 2) % FC_NF) + FC_NF) % FC_NF;
            hh = FC_NF - FC_NI / 2;
        }
        float xr = s->X[2 * src], xi = s->X[2 * src + 1];
        float hr = fc_H[2 * hh], hi = fc_H[2 * hh + 1];
        l->y[2 * j] = xr * hr - xi * hi;
        l->y[2 * j + 1] = xr * hi + xi * hr;
        if (++src == FC_NF) src = 0;
        if (++hh == FC_NF) hh = 0;
    }
    fftwf_execute_dft(fc_plan_i, (fftwf_complex *)l->y, (fftwf_complex *)l->y);
    /* taking bins around kc mixed every window by kc from the window start; the window
       start moves FC_HOP samples per window: undo that phase so the mixing is continuous */
    long long kcm = ((long long)l->kc % FC_NF + FC_NF) % FC_NF;
    long long ph = (kcm * FC_HOP % FC_NF) * (long long)((s->m - 1) % FC_NF) % FC_NF;
    float cr = (float)cos(-2 * M_PI * ph / FC_NF), ci = (float)sin(-2 * M_PI * ph / FC_NF);
    float rr = l->rr, ri = l->ri;
    for (int p = 0; p < FC_OUT; p++) {
        float yr = l->y[2 * (FC_NI - FC_OUT + p)], yi = l->y[2 * (FC_NI - FC_OUT + p) + 1];
        float ar = yr * cr - yi * ci, ai = yr * ci + yi * cr;
        out[2 * p] = ar * rr - ai * ri;
        out[2 * p + 1] = ar * ri + ai * rr;
        float t = rr * l->sr - ri * l->si;
        ri = rr * l->si + ri * l->sr;
        rr = t;
    }
    float g = 1.0f / sqrtf(rr * rr + ri * ri);
    l->rr = rr * g;
    l->ri = ri * g;
    return FC_OUT;
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
    d->wfill = CF_N - CF_HOP;
    double h[DM_NA];
    lowpass(h, DM_NA, 3600.0 / FS_NARROW);
    for (int n = 0; n < DM_NA; n++)
        d->ataps[n] = (float)h[DM_NA - 1 - n];
    d->peak = 1e-4f;
    d->sql = -999;
    demod_set(d, M_USB, 300, 2700);
}

/*
 * Taps for a passband width: a Blackman filter's skirts are ~5.5 fs / N wide
 * (161 taps: ~550 Hz), fine for SSB but as wide as a CW filter itself. Narrow
 * passbands get longer, steeper filters (641 taps: ~140 Hz), at up to four
 * times the CPU of the channel filter for that listener.
 */
static int demod_taps(float width)
{
    if (width >= 1700) return 161;
    if (width >= 1000) return 241;
    if (width >= 400)  return 401;
    return 641;
}

void demod_set(demod_t *d, int mode, float lo, float hi)
{
    static double h[DM_NT];
    int nt = demod_taps(hi - lo);
    double fc = (hi - lo) / 2.0 / FS_NARROW;
    double f0 = (hi + lo) / 2.0 / FS_NARROW;
    int M = nt / 2;

    d->nt = nt;
    lowpass(h, nt, fc);
    /* shift the lowpass to the passband centre, zero-pad, transform; 1/CF_N for the inverse */
    static float t[2 * CF_N];
    memset(t, 0, sizeof(t));
    for (int n = 0; n < nt; n++) {
        int m = n - M;
        t[2 * n] = (float)(h[n] * cos(2 * M_PI * f0 * m));
        t[2 * n + 1] = (float)(h[n] * sin(2 * M_PI * f0 * m));
    }
    fftwf_execute_dft(cf_plan_f, (fftwf_complex *)t, (fftwf_complex *)d->H);
    for (int k = 0; k < 2 * CF_N; k++)
        d->H[k] /= CF_N;
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

/* one channel-filtered sample: detector, AGC, squelch, 16 -> 8 kS/s */
static void demod_sample(demod_t *d, float yr, float yi)
{
    {
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
            return;
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
 * Channel filter by overlap-save: every CF_HOP new samples one forward and one inverse
 * CF_N-point FFT; the last CF_HOP outputs are valid (filter length <= DM_NT).
 */
void demod_process(demod_t *d, const float *iq, int n)
{
    int i = 0;
    while (i < n) {
        int k = CF_N - d->wfill;
        if (k > n - i)
            k = n - i;
        memcpy(d->win + 2 * d->wfill, iq + 2 * i, 2 * k * sizeof(float));
        d->wfill += k;
        i += k;
        if (d->wfill < CF_N)
            break;
        fftwf_execute_dft(cf_plan_f, (fftwf_complex *)d->win, (fftwf_complex *)d->tmp);
        for (int j = 0; j < CF_N; j++) {
            float ar = d->tmp[2 * j], ai = d->tmp[2 * j + 1];
            float hr = d->H[2 * j], hi = d->H[2 * j + 1];
            d->tmp[2 * j] = ar * hr - ai * hi;
            d->tmp[2 * j + 1] = ar * hi + ai * hr;
        }
        fftwf_execute_dft(cf_plan_i, (fftwf_complex *)d->tmp, (fftwf_complex *)d->tmp2);
        for (int j = CF_N - CF_HOP; j < CF_N; j++)
            demod_sample(d, d->tmp2[2 * j], d->tmp2[2 * j + 1]);
        memmove(d->win, d->win + 2 * CF_HOP, 2 * (CF_N - CF_HOP) * sizeof(float));
        d->wfill = CF_N - CF_HOP;
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
