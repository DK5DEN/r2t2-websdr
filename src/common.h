#ifndef COMMON_H
#define COMMON_H

#define NRX          8        /* hardware receivers in the FPGA */
#define FS_NARROW    16000    /* per-receiver rate of the narrow (audio) stream */
#define FS_WIDE      192000   /* per-receiver rate of the wide (waterfall) stream */
#define WF_FFT       4096     /* waterfall FFT size */
#define WF_BINS      1024     /* bins sent to the browser per waterfall line */
#define AUDIO_RATE   8000     /* audio rate sent to the browser */
#define AUDIO_BLOCK  256      /* audio samples per websocket packet */
#define CW_PITCH     700.0    /* CW beat note in Hz */
#define MAX_BANDS    32
#define MAX_CLIENTS  32

#endif
