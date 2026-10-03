#ifndef DSP_H
#define DSP_H

#include <stdint.h>
#include <fftw3.h>

#include "common.h"

#define DM_NT      161     /* channel filter taps at 16 kS/s */
#define DM_NA      31      /* audio anti-alias taps before 16k -> 8k */
#define DM_OUTMAX  2048

enum { M_USB, M_LSB, M_CW, M_AM, M_FM };

/* waterfall: averaged power spectrum of one wide receiver */
typedef struct {
    fftwf_complex *in, *out;
    float acc[WF_FFT];
    int fill, nacc, navg;
} wf_t;

typedef struct {
    int16_t pred;
    int index;
} ima_t;

/* demodulator for one narrow receiver */
typedef struct {
    int mode;
    float lo, hi;                       /* passband in Hz relative to the receiver NCO */
    float tr[DM_NT], ti[DM_NT];         /* complex channel filter, reversed */
    float xr[2 * DM_NT], xi[2 * DM_NT]; /* doubled history */
    int hp;
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

int dsp_global_init(void);

int wf_init(wf_t *w, int navg);
void wf_reset(wf_t *w);
int wf_push(wf_t *w, const float *iq, int n, uint8_t *frame);

void demod_init(demod_t *d);
void demod_set(demod_t *d, int mode, float lo, float hi);
void demod_process(demod_t *d, const float *iq, int n);
int demod_packet(demod_t *d, uint8_t *pkt);
float demod_level_db(const demod_t *d);

#endif
