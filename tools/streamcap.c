/*
 * Diagnose the FPGA streams on rad0 next to the running server (a packet socket gets its
 * own copy of every frame).
 *   sudo ./streamcap [seconds]        frames/s, words per frame, tag bytes per stream
 *   sudo ./streamcap pairs <stream>   record 16384 sample groups of a stream and, for every
 *                                     receiver, show the strongest line and how clean it is
 *                                     under several word layouts
 */
#include <arpa/inet.h>
#include <complex.h>
#include <linux/if_packet.h>
#include <math.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define ETH_P_R2T2 0x7232
#define NG 16384

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static int open_rad0(void)
{
    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_R2T2));
    if (fd < 0) { perror("socket"); exit(1); }
    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_R2T2);
    sll.sll_ifindex = if_nametoindex("rad0");
    if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) { perror("bind"); exit(1); }
    return fd;
}

static float s24(uint32_t w) { return (float)((int32_t)(w << 8) >> 8) * (1.0f / 8388608.0f); }

static void fft(float complex *x, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float complex t = x[i]; x[i] = x[j]; x[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float complex wl = cexpf(-2 * M_PI * I / len);
        for (int i = 0; i < n; i += len) {
            float complex w = 1;
            for (int k = 0; k < len / 2; k++) {
                float complex u = x[i + k], v = x[i + k + len / 2] * w;
                x[i + k] = u + v;
                x[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

/* strongest line (Hz) and its share of the energy within +-200 Hz (1 = clean line) */
static void line(const float complex *x0, double fs, double *f, double *pur)
{
    static float complex x[NG];
    for (int i = 0; i < NG; i++) x[i] = x0[i] * (0.5f - 0.5f * cosf(2 * M_PI * i / NG));
    fft(x, NG);
    int best = 1;
    for (int i = 1; i < NG; i++) if (cabsf(x[i]) > cabsf(x[best]) && i != NG / 2) best = i;
    double df = fs / NG, near = 0, wide = 0;
    for (int d = -(int)(200 / df); d <= (int)(200 / df); d++) {
        double p = cabsf(x[(best + d + NG) % NG]); p *= p;
        wide += p;
        if (abs(d) <= 3) near += p;
    }
    *f = (best < NG / 2 ? best : best - NG) * df;
    *pur = near / wide;
}

int main(int argc, char **argv)
{
    int fd = open_rad0();
    static uint8_t buf[65536];

    if (argc > 1 && !strcmp(argv[1], "compare")) {
        /* narrow and wide stream of the same receiver share the NCO: the wide one,
           decimated to 16 kS/s, must match the narrow one. Correlate both ways. */
        enum { NN = 4096, NW = NN * 12 + 4000 * 12, LAG = 1500 };
        static float complex nar[8][NN], wid[8][NW / 12];
        enum { NT = 191 };
        static float complex hist[8][NT];
        static float taps[NT];
        double ts = 0;
        for (int n = 0; n < NT; n++) {   /* low-pass 7 kHz at 192 kS/s */
            double m = n - (NT - 1) / 2.0, fc = 7000.0 / 192000;
            taps[n] = (float)((m == 0 ? 2 * fc : sin(2 * M_PI * fc * m) / (M_PI * m)) *
                      (0.42 - 0.5 * cos(2 * M_PI * n / (NT - 1)) + 0.08 * cos(4 * M_PI * n / (NT - 1))));
            ts += taps[n];
        }
        for (int n = 0; n < NT; n++) taps[n] /= ts;
        int hp = 0;
        int nn = 0, nwid = 0, phase = 0;
        while (nn < NN || nwid < NW / 12) {
            int len = recv(fd, buf, sizeof(buf), 0);
            if (len <= 16) continue;
            const uint32_t *w = (const uint32_t *)(buf + 16);
            int nw = (len - 16) / 4, st = (w[0] >> 24) & 7, per = nw / 16;
            if (st == 1)
                for (int i = 0; i < per && nn < NN; i++, nn++)
                    for (int k = 0; k < 8; k++)
                        nar[k][nn] = s24(w[2 * (i * 8 + k)]) + I * s24(w[2 * (i * 8 + k) + 1]);
            if (st == 2)
                for (int i = 0; i < per && nwid < NW / 12; i++) {
                    for (int k = 0; k < 8; k++)
                        hist[k][hp] = s24(w[2 * (i * 8 + k)]) + I * s24(w[2 * (i * 8 + k) + 1]);
                    hp = (hp + 1) % NT;
                    if (++phase == 12) {
                        phase = 0;
                        for (int k = 0; k < 8; k++) {
                            float complex s = 0;
                            for (int n = 0; n < NT; n++) s += taps[n] * hist[k][(hp + n) % NT];
                            wid[k][nwid] = s;
                        }
                        nwid++;
                    }
                }
        }
        printf("rx | straight (I,Q): corr  lag | swapped (Q,I): corr  lag\n");
        for (int k = 0; k < 8; k++) {
            double best[2] = { 0, 0 };
            int lag[2] = { 0, 0 };
            double en = 0, ew = 0;
            for (int i = 0; i < NN; i++) en += crealf(nar[k][i] * conjf(nar[k][i]));
            for (int sw = 0; sw < 2; sw++)
                for (int l = 0; l < LAG * 2; l++) {
                    float complex c = 0;
                    double e = 0;
                    for (int i = 0; i < NN; i++) {
                        float complex a = sw ? conjf(nar[k][i]) * I : nar[k][i];   /* (Q,I) = i*conj(I+iQ) */
                        float complex b = wid[k][i + l];
                        c += a * conjf(b);
                        e += crealf(b * conjf(b));
                    }
                    double r = cabs(c) / sqrt(en * e + 1e-30);
                    if (r > best[sw]) { best[sw] = r; lag[sw] = l - LAG; }
                }
            (void)ew;
            printf("%d  | %.3f %5d | %.3f %5d\n", k, best[0], lag[0], best[1], lag[1]);
        }
        /* every narrow receiver against every wide one, both I/Q orders */
        const int N2 = 2048, L2 = 600;
        printf("narrow rx -> best wide rx (corr, lag, order)\n");
        for (int k = 0; k < 8; k++) {
            double en = 0, best = 0;
            int bj = -1, bl = 0, bs = 0;
            for (int i = 0; i < N2; i++) en += crealf(nar[k][i] * conjf(nar[k][i]));
            for (int j = 0; j < 8; j++)
                for (int sw = 0; sw < 2; sw++)
                    for (int l = 0; l < 2 * L2; l++) {
                        float complex c = 0;
                        double e = 0;
                        for (int i = 0; i < N2; i++) {
                            float complex a = sw ? conjf(nar[k][i]) * I : nar[k][i];
                            float complex b = wid[j][i + l];
                            c += a * conjf(b);
                            e += crealf(b * conjf(b));
                        }
                        double r = cabs(c) / sqrt(en * e + 1e-30);
                        if (r > best) { best = r; bj = j; bl = l - L2; bs = sw; }
                    }
            printf("%d -> %d  corr %.3f  lag %d  %s\n", k, bj, best, bl, bs ? "(Q,I)" : "(I,Q)");
        }
        return 0;
    }

    if (argc > 2 && !strcmp(argv[1], "pairs")) {
        int want = atoi(argv[2]);
        double fs = want == 1 ? 16000 : 192000;
        static uint32_t grp[NG][16], blk[NG][16];
        int g = 0, gb = 0;
        while (g < NG) {
            int len = recv(fd, buf, sizeof(buf), 0);
            if (len <= 16) continue;
            const uint32_t *w = (const uint32_t *)(buf + 16);
            int nw = (len - 16) / 4;
            if (((w[0] >> 24) & 7) != want) continue;
            int per = nw / 16;
            /* receiver-major per frame: rx k's samples are words 2*per*k .. 2*per*(k+1)-1 */
            for (int i = 0; i < per && gb < NG; i++, gb++)
                for (int k = 0; k < 8; k++) {
                    blk[gb][2 * k] = w[2 * per * k + 2 * i];
                    blk[gb][2 * k + 1] = w[2 * per * k + 2 * i + 1];
                }
            for (int i = 0; i + 16 <= nw && g < NG; i += 16, g++)
                memcpy(grp[g], w + i, 64);
        }
        static float complex z[NG];
        printf("stream %d, %d sample groups\n", want, NG);
        printf("rx | (2k,2k+1) as today | (2k+1,2k) I/Q swapped | (k,k+8) I-block/Q-block\n");
        for (int k = 0; k < 8; k++) {
            double f[4], p[4];
            for (int g2 = 0; g2 < NG; g2++) z[g2] = s24(grp[g2][2 * k]) + I * s24(grp[g2][2 * k + 1]);
            line(z, fs, &f[0], &p[0]);
            for (int g2 = 0; g2 < NG; g2++) z[g2] = s24(grp[g2][2 * k + 1]) + I * s24(grp[g2][2 * k]);
            line(z, fs, &f[1], &p[1]);
            for (int g2 = 0; g2 < NG; g2++) z[g2] = s24(grp[g2][k]) + I * s24(grp[g2][k + 8]);
            line(z, fs, &f[2], &p[2]);
            for (int g2 = 0; g2 < NG; g2++) z[g2] = s24(blk[g2][2 * k]) + I * s24(blk[g2][2 * k + 1]);
            line(z, fs, &f[3], &p[3]);
            printf("%d  | %8.1f Hz %.3f | %8.1f Hz %.3f | %8.1f Hz %.3f | %8.1f Hz %.3f\n",
                   k, f[0], p[0], f[1], p[1], f[2], p[2], f[3], p[3]);
        }
        return 0;
    }

    double secs = argc > 1 ? atof(argv[1]) : 3;
    long frames[8] = { 0 }, words[8] = { 0 }, minw[8], maxw[8];
    for (int i = 0; i < 8; i++) { minw[i] = 1 << 30; maxw[i] = 0; }
    int shown[8] = { 0 };
    long tagcount[256] = { 0 };
    double t0 = now();
    while (now() - t0 < secs) {
        int len = recv(fd, buf, sizeof(buf), 0);
        if (len <= 16) continue;
        const uint32_t *w = (const uint32_t *)(buf + 16);
        int nw = (len - 16) / 4;
        int st = (w[0] >> 24) & 7;
        frames[st]++;
        words[st] += nw;
        if (nw < minw[st]) minw[st] = nw;
        if (nw > maxw[st]) maxw[st] = nw;
        for (int i = 0; i < nw; i++) tagcount[w[i] >> 24]++;
        if (shown[st] < 1) {
            shown[st]++;
            printf("stream %d frame: len %d, %d words; tags of the first 32 words:", st, len, nw);
            for (int i = 0; i < 32 && i < nw; i++) printf(" %02x", w[i] >> 24);
            printf("\n");
        }
    }
    double dt = now() - t0;
    for (int st = 0; st < 8; st++) {
        if (!frames[st]) continue;
        double wps = words[st] / dt;
        printf("stream %d: %.0f frames/s, %ld..%ld words/frame, %.0f IQ samples/s per receiver\n",
               st, frames[st] / dt, minw[st], maxw[st], wps / 2 / 8);
    }
    printf("tag bytes seen:");
    for (int t = 0; t < 256; t++) if (tagcount[t]) printf(" %02x:%ld", t, tagcount[t]);
    printf("\n");
    return 0;
}
