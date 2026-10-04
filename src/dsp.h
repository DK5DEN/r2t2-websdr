#ifndef DSP_H
#define DSP_H

#include <stdint.h>
#include <fftw3.h>

#include "common.h"

/* channel filter at 16 kS/s by fast convolution (overlap-save): CF_N-point FFTs, CF_HOP new
   samples per block, filters up to DM_NT taps; the cost does not depend on the length */
#define CF_N       1024
#define CF_HOP     384
#define DM_NT      (CF_N - CF_HOP + 1)   /* 641 */
#define DM_NA      31      /* audio anti-alias taps before 16k -> 8k */
#define DM_OUTMAX  2048

enum { M_USB, M_LSB, M_CW, M_AM, M_FM };

/* waterfall: averaged power spectrum of one wide receiver */
#define WF_AVG 2   /* FFTs averaged per line; the rest of the line period is skipped */
typedef struct {
    fftwf_complex *in, *out;
    float acc[WF_FFT];
    int fill, nacc, navg;
    int pos, period;   /* sample position within one line period */
    float spec[WF_FFT];  /* last line, power per FFT bin, lowest frequency first (for zoom) */
} wf_t;

typedef struct {
    int16_t pred;
    int index;
} ima_t;

/* demodulator for one narrow receiver */
typedef struct {
    int mode;
    float lo, hi;                       /* passband in Hz relative to the receiver NCO */
    float H[2 * CF_N];                  /* channel filter spectrum, scaled by 1/CF_N */
    float win[2 * CF_N];                /* input window: DM_NT-1 old + CF_HOP new samples */
    float tmp[2 * CF_N], tmp2[2 * CF_N];
    int wfill, nt;                      /* samples in win, taps of the current filter */
    float ah[2 * DM_NA], ataps[DM_NA];
    int ap, dec;
    float pr, pi;                       /* FM: previous sample */
    float dc;                           /* AM: carrier level */
    float peak;                         /* AGC */
    int hang;
    float pwr;                          /* smoothed channel power, full scale = 1 */
    float sql;                          /* squelch level in dBFS, <= -200 = off */
    int sqopen;
    ima_t ima;
    int16_t out[DM_OUTMAX];
    int outn;
} demod_t;

/*
 * Listeners from the wide stream by fast convolution: one FC_NF-point FFT per segment
 * (overlap-save, FC_HOP new samples per window) is shared by all its listeners; each one
 * takes the FC_NI bins around its frequency, weighted by an anti-alias low-pass, and an
 * FC_NI-point inverse FFT gives FC_OUT samples at 16 kS/s.
 */
#define DDC_DEC  (FS_WIDE / FS_NARROW)   /* 12 */
#define FC_NF    6144
#define FC_HOP   4608
#define FC_NI    (FC_NF / DDC_DEC)        /* 512 */
#define FC_OUT   (FC_HOP / DDC_DEC)       /* 384 */
#define FC_LP    (FC_NF - FC_HOP + 1)     /* low-pass taps at 192 kS/s */

typedef struct {
    float x[2 * FC_NF];                 /* the last FC_NF input samples, oldest first */
    float X[2 * FC_NF];                 /* spectrum of the last full window */
    int fill;                           /* samples in x */
    int ready;                          /* X is new: listeners take their part now */
    unsigned long m;                    /* windows transformed so far */
} fcseg_t;

typedef struct {
    int kc;                             /* bin nearest to the listener frequency */
    float rr, ri, sr, si;               /* residual shift below one bin: phasor and step */
    float y[2 * FC_NI];
} fclis_t;

int dsp_global_init(void);

void fc_seg_reset(fcseg_t *s);
int fc_seg_feed(fcseg_t *s, const float *iq, int n);
void fc_set(fclis_t *l, double offset_hz);
int fc_out(const fcseg_t *s, fclis_t *l, float *out);

int wf_init(wf_t *w, int navg);
void wf_reset(wf_t *w);
int wf_push(wf_t *w, const float *iq, int n, uint8_t *frame);
void wf_zoom(const wf_t *w, double rel0, double df, int n, uint8_t *out);

void demod_init(demod_t *d);
void demod_set(demod_t *d, int mode, float lo, float hi);
void demod_process(demod_t *d, const float *iq, int n);
int demod_packet(demod_t *d, uint8_t *pkt);
float demod_level_db(const demod_t *d);

#endif
