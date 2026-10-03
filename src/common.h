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

#include <string.h>

/*
 * Drop a UTF-8 sequence cut off at the end of s. Browsers close a WebSocket
 * on invalid UTF-8 in a text frame, so every shortened string goes through here.
 */
static inline void utf8_trim(char *s)
{
    size_t n = strlen(s), i = n;
    while (i > 0 && ((unsigned char)s[i - 1] & 0xc0) == 0x80)
        i--;                                   /* continuation bytes */
    if (i == 0)
        return;
    unsigned char lead = (unsigned char)s[i - 1];
    size_t need = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
    if (n - (i - 1) < need)
        s[i - 1] = 0;
}

#endif
