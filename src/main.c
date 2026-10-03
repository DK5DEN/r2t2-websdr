/*
 * r2t2sdr - multi-user WebSDR for the DARC R2T2 receiver.
 *
 * Every waterfall view in use occupies one FPGA receiver (192 kS/s stream),
 * every listener occupies one more (16 kS/s stream). Channel selection is done
 * by the FPGA; the CPU only computes the waterfall FFT and demodulates audio.
 *
 * A view is either one of the configured bands or a free centre frequency
 * (used when the page follows an external rig). Clients on the same view share
 * its receiver.
 */
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "config.h"
#include "dsp.h"
#include "hw.h"
#include "net.h"

#define MAX_VIEWS      (MAX_BANDS + NRX)
#define CLIENT_TIMEOUT 30
#define VERSION        "0.2.0"

typedef struct {
    int used;      /* configured band, or free view with viewers */
    int band;      /* configured band index, -1 for a free view */
    double center;
    int input;
    int rx;        /* receiver feeding the waterfall, -1 = inactive */
    int viewers;
    wf_t wf;
} view_t;

static config_t cfg;
static client_t cl[MAX_CLIENTS];
static view_t views[MAX_VIEWS];
static int nviews;
static int rx_view[NRX];
static int rx_client[NRX];
static volatile sig_atomic_t running = 1;
static int verbose;

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static long now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

static void on_signal(int sig)
{
    (void)sig;
    running = 0;
}

/* ---------------------------------------------------------------- helpers */

static void json_escape(char *dst, size_t n, const char *s)
{
    size_t o = 0;
    for (; *s && o + 7 < n; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') {
            dst[o++] = '\\';
            dst[o++] = ch;
        } else if (ch < 0x20) {
            o += snprintf(dst + o, n - o, "\\u%04x", ch);
        } else {
            dst[o++] = ch;
        }
    }
    dst[o] = 0;
}

static const char *json_find(const char *s, const char *key)
{
    char k[48];
    snprintf(k, sizeof(k), "\"%s\"", key);
    const char *p = strstr(s, k);
    if (!p)
        return NULL;
    p += strlen(k);
    while (*p == ' ')
        p++;
    if (*p != ':')
        return NULL;
    p++;
    while (*p == ' ')
        p++;
    return p;
}

static int json_num(const char *s, const char *key, double *v)
{
    const char *p = json_find(s, key);
    if (!p)
        return 0;
    char *e;
    *v = strtod(p, &e);
    return e != p;
}

static int json_str(const char *s, const char *key, char *out, size_t n)
{
    const char *p = json_find(s, key);
    if (!p || *p != '"')
        return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n)
        out[i++] = *p++;
    out[i] = 0;
    return 1;
}

static void send_text(client_t *c, const char *s)
{
    net_ws_send(c, 1, s, strlen(s), 0);
}

static void send_error(client_t *c, const char *msg)
{
    char b[256], e[160];
    json_escape(e, sizeof(e), msg);
    snprintf(b, sizeof(b), "{\"type\":\"error\",\"msg\":\"%s\"}", e);
    send_text(c, b);
}

static const char *mode_name(int m)
{
    static const char *n[] = { "usb", "lsb", "cw", "am", "fm" };
    return (m >= 0 && m <= M_FM) ? n[m] : "usb";
}

static int mode_parse(const char *s)
{
    if (!strcmp(s, "nfm"))
        return M_FM;
    for (int m = 0; m <= M_FM; m++)
        if (!strcmp(s, mode_name(m)))
            return m;
    return -1;
}

static int rx_alloc(void)
{
    for (int rx = 0; rx < NRX; rx++)
        if (rx_view[rx] < 0 && rx_client[rx] < 0)
            return rx;
    return -1;
}

static int rx_free_count(void)
{
    int n = 0;
    for (int rx = 0; rx < NRX; rx++)
        if (rx_view[rx] < 0 && rx_client[rx] < 0)
            n++;
    return n;
}

static int count_users(int *listeners)
{
    int users = 0, l = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (cl[i].fd >= 0 && cl[i].ws) {
            users++;
            if (cl[i].listening)
                l++;
        }
    }
    if (listeners)
        *listeners = l;
    return users;
}

static int status_json(char *b, size_t n)
{
    int listeners, users = count_users(&listeners);
    return snprintf(b, n, "{\"type\":\"status\",\"users\":%d,\"listeners\":%d,\"free\":%d,\"total\":%d}",
                    users, listeners, rx_free_count(), NRX);
}

static void broadcast_status(void)
{
    char b[160];
    status_json(b, sizeof(b));
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (cl[i].fd >= 0 && cl[i].ws)
            send_text(&cl[i], b);
}

static int config_json(char *b, size_t n)
{
    char t[200], cs[64], loc[200], lc[32];
    json_escape(t, sizeof(t), cfg.title);
    json_escape(cs, sizeof(cs), cfg.callsign);
    json_escape(loc, sizeof(loc), cfg.location);
    json_escape(lc, sizeof(lc), cfg.locator);
    int o = snprintf(b, n,
                     "{\"type\":\"config\",\"version\":\"%s\",\"title\":\"%s\",\"callsign\":\"%s\","
                     "\"location\":\"%s\",\"locator\":\"%s\",\"span\":%d,\"bins\":%d,\"audioRate\":%d,"
                     "\"receivers\":%d,\"maxFreq\":%.0f,\"bands\":[",
                     VERSION, t, cs, loc, lc, FS_WIDE, WF_BINS, AUDIO_RATE, NRX, cfg.clock / 2);
    for (int i = 0; i < cfg.nbands && o < (int)n - 200; i++) {
        char nm[80];
        json_escape(nm, sizeof(nm), cfg.bands[i].name);
        o += snprintf(b + o, n - o, "%s{\"id\":%d,\"name\":\"%s\",\"center\":%.0f,\"mode\":\"%s\"}",
                      i ? "," : "", i, nm, cfg.bands[i].center, cfg.bands[i].mode);
    }
    o += snprintf(b + o, n - o, "]}");
    return o;
}

/* ---------------------------------------------------------------- views */

static void view_leave(client_t *c)
{
    if (c->view < 0)
        return;
    view_t *v = &views[c->view];
    if (--v->viewers <= 0) {
        v->viewers = 0;
        if (v->rx >= 0) {
            rx_view[v->rx] = -1;
            v->rx = -1;
        }
        if (v->band < 0)
            v->used = 0;
    }
    c->view = -1;
}

static void send_view(client_t *c)
{
    view_t *v = &views[c->view];
    char m[200];
    snprintf(m, sizeof(m), "{\"type\":\"view\",\"id\":%d,\"band\":%d,\"center\":%.0f,\"span\":%d}",
             c->view, v->band, v->center, FS_WIDE);
    send_text(c, m);
}

static void view_join(client_t *c, int id)
{
    view_t *v = &views[id];
    if (c->view != id) {
        /* leave first so a view seen only by this client frees its receiver */
        view_leave(c);
        if (v->rx < 0) {
            int rx = rx_alloc();
            if (rx < 0) {
                if (v->band < 0 && v->viewers == 0)
                    v->used = 0;
                send_error(c, "Kein Empfänger für den Wasserfall frei");
                return;
            }
            v->rx = rx;
            rx_view[rx] = id;
            hw_set_input(rx, v->input);
            hw_set_freq(rx, v->center);
            wf_reset(&v->wf);
        }
        v->used = 1;
        v->viewers++;
        c->view = id;
    }
    send_view(c);
    broadcast_status();
}

/* antenna input of the configured band covering f, else input 1 */
static int input_for(double f)
{
    for (int b = 0; b < cfg.nbands; b++)
        if (fabs(f - cfg.bands[b].center) < FS_WIDE / 2)
            return cfg.bands[b].input;
    return 1;
}

/* Free centre frequency: share an existing view with that centre or open a new one. */
static void view_center(client_t *c, double center)
{
    if (center < FS_WIDE / 2)
        center = FS_WIDE / 2;
    if (center > cfg.clock / 2 - FS_WIDE / 2)
        center = cfg.clock / 2 - FS_WIDE / 2;
    for (int i = 0; i < nviews; i++)
        if (views[i].used && fabs(views[i].center - center) < 1) {
            view_join(c, i);
            return;
        }
    for (int i = cfg.nbands; i < nviews; i++) {
        view_t *v = &views[i];
        if (v->used || i == c->view)
            continue;
        v->center = center;
        v->input = input_for(center);
        v->rx = -1;
        v->viewers = 0;
        v->used = 1;
        view_join(c, i);
        return;
    }
    send_error(c, "Zu viele Ansichten offen");
}

/* ---------------------------------------------------------------- listeners */

static void apply_tune(client_t *c)
{
    if (c->rx < 0)
        return;
    float lo = c->lo, hi = c->hi;
    double f = c->freq;
    if (c->mode == M_CW) {
        /* signal lands at +CW_PITCH, real part gives the beat note */
        f -= CW_PITCH;
        lo += CW_PITCH;
        hi += CW_PITCH;
    }
    hw_set_freq(c->rx, f);
    demod_set(&c->dm, c->mode, lo, hi);
}

static void send_audio_state(client_t *c)
{
    char m[64];
    snprintf(m, sizeof(m), "{\"type\":\"audio\",\"on\":%s}", c->listening ? "true" : "false");
    send_text(c, m);
}

static void listener_stop(client_t *c)
{
    if (c->rx >= 0) {
        rx_client[c->rx] = -1;
        c->rx = -1;
    }
    c->listening = 0;
}

static void listener_start(client_t *c)
{
    if (c->rx < 0) {
        int rx = rx_alloc();
        if (rx < 0) {
            send_error(c, "Alle Empfänger belegt, bitte später erneut versuchen");
            send_audio_state(c);
            return;
        }
        c->rx = rx;
        rx_client[rx] = (int)(c - cl);
        hw_set_input(rx, input_for(c->freq));
        demod_init(&c->dm);
        c->dm.sql = c->sql;
        apply_tune(c);
    }
    c->listening = 1;
    send_audio_state(c);
    broadcast_status();
}

/* ---------------------------------------------------------------- websocket */

static void on_open(client_t *c)
{
    c->view = -1;
    c->rx = -1;
    c->listening = 0;
    c->freq = cfg.nbands ? cfg.bands[0].center : 7100000;
    c->mode = M_LSB;
    c->lo = -2700;
    c->hi = -300;
    c->sql = -999;
    logmsg("client %s connected", c->ip);
    static char b[8192];
    config_json(b, sizeof(b));
    send_text(c, b);
    broadcast_status();
}

static void on_text(client_t *c, char *txt, size_t len)
{
    (void)len;
    char cmd[16];
    double v;
    if (!json_str(txt, "cmd", cmd, sizeof(cmd)))
        return;

    if (!strcmp(cmd, "band")) {
        if (json_num(txt, "id", &v) && v >= 0 && v < cfg.nbands)
            view_join(c, (int)v);
        else
            send_error(c, "Unbekanntes Band");
    } else if (!strcmp(cmd, "view")) {
        if (json_num(txt, "center", &v))
            view_center(c, v);
    } else if (!strcmp(cmd, "tune")) {
        char ms[8];
        double f, lo, hi;
        if (!json_num(txt, "freq", &f) || f < 0 || f > cfg.clock / 2) {
            send_error(c, "Frequenz außerhalb des Empfangsbereichs");
            return;
        }
        if (json_str(txt, "mode", ms, sizeof(ms))) {
            int m = mode_parse(ms);
            if (m >= 0)
                c->mode = m;
        }
        if (json_num(txt, "lo", &lo) && json_num(txt, "hi", &hi) &&
            lo < hi && hi - lo >= 50 && lo >= -7900 && hi <= 7900) {
            c->lo = (float)lo;
            c->hi = (float)hi;
        }
        c->freq = f;
        apply_tune(c);
    } else if (!strcmp(cmd, "start")) {
        listener_start(c);
    } else if (!strcmp(cmd, "stop")) {
        listener_stop(c);
        send_audio_state(c);
        broadcast_status();
    } else if (!strcmp(cmd, "squelch")) {
        if (json_num(txt, "level", &v)) {
            c->sql = (float)v;
            c->dm.sql = c->sql;
            c->dm.sqopen = 0;
        }
    }
    /* "ping" needs no answer, receiving it refreshes last_rx */
}

/* read-only JSON endpoints for scripts and monitoring */
static int on_http(client_t *c, const char *path)
{
    static char b[8192];
    if (!strcmp(path, "/api/status")) {
        int o = status_json(b, sizeof(b));
        o--; /* reopen the object to append the views */
        o += snprintf(b + o, sizeof(b) - o, ",\"views\":[");
        int first = 1;
        for (int i = 0; i < nviews; i++) {
            if (!views[i].viewers)
                continue;
            o += snprintf(b + o, sizeof(b) - o, "%s{\"id\":%d,\"band\":%d,\"center\":%.0f,\"viewers\":%d}",
                          first ? "" : ",", i, views[i].band, views[i].center, views[i].viewers);
            first = 0;
        }
        o += snprintf(b + o, sizeof(b) - o, "]}");
        net_http_reply(c, 200, "application/json", b, o);
        return 1;
    }
    if (!strcmp(path, "/api/config")) {
        int o = config_json(b, sizeof(b));
        net_http_reply(c, 200, "application/json", b, o);
        return 1;
    }
    return 0;
}

static const net_cb_t callbacks = { on_open, on_text, on_http };

static void client_close(client_t *c)
{
    if (c->fd < 0)
        return;
    int was_ws = c->ws;
    if (was_ws)
        logmsg("client %s disconnected", c->ip);
    view_leave(c);
    listener_stop(c);
    close(c->fd);
    free(c->out);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    c->view = -1;
    c->rx = -1;
    if (was_ws)
        broadcast_status();
}

/* ---------------------------------------------------------------- stream */

static inline float s24(uint32_t w)
{
    return (float)((int32_t)(w << 8) >> 8) * (1.0f / 8388608.0f);
}

/*
 * Stream frame: 16 byte header (Ethernet + padding), then 32-bit words.
 * Bits 0..23 sample, bits 24..31 tag; tag & 7 = 1 narrow stream, 2 wide stream.
 * Samples are I,Q pairs of receivers 0..7, interleaved.
 */
static void on_packet(const uint8_t *buf, int len)
{
    static float iq[NRX][2 * 128];
    static uint8_t frame[2 + WF_BINS];
    static uint8_t apkt[8 + AUDIO_BLOCK];

    if (len <= 16)
        return;
    const uint32_t *w = (const uint32_t *)(buf + 16);
    int nw = (len - 16) / 4;
    int stream = (w[0] >> 24) & 7;
    int per = nw / 2 / NRX;
    if (per <= 0 || per > 128 || (stream != 1 && stream != 2))
        return;

    int need[NRX], any = 0;
    for (int rx = 0; rx < NRX; rx++) {
        need[rx] = stream == 2 ? rx_view[rx] >= 0 : rx_client[rx] >= 0;
        any |= need[rx];
    }
    if (!any)
        return;

    for (int g = 0; g < per; g++)
        for (int rx = 0; rx < NRX; rx++)
            if (need[rx]) {
                const uint32_t *p = w + 2 * (g * NRX + rx);
                iq[rx][2 * g] = s24(p[0]);
                iq[rx][2 * g + 1] = s24(p[1]);
            }

    for (int rx = 0; rx < NRX; rx++) {
        if (!need[rx])
            continue;
        if (stream == 2) {
            int v = rx_view[rx];
            if (wf_push(&views[v].wf, iq[rx], per, frame + 2)) {
                frame[0] = 1;
                frame[1] = (uint8_t)v;
                for (int i = 0; i < MAX_CLIENTS; i++)
                    if (cl[i].fd >= 0 && cl[i].ws && cl[i].view == v)
                        net_ws_send(&cl[i], 2, frame, sizeof(frame), 1);
            }
        } else {
            client_t *c = &cl[rx_client[rx]];
            demod_process(&c->dm, iq[rx], per);
            int n;
            while ((n = demod_packet(&c->dm, apkt)) > 0)
                net_ws_send(c, 2, apkt, n, 1);
        }
    }
}

/* ---------------------------------------------------------------- main */

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s [-c config] [-v]\n", prog);
}

int main(int argc, char **argv)
{
    const char *cfgpath = "/etc/r2t2sdr.conf";
    int opt;
    while ((opt = getopt(argc, argv, "c:vh")) != -1) {
        switch (opt) {
        case 'c': cfgpath = optarg; break;
        case 'v': verbose = 1; break;
        default: usage(argv[0]); return 1;
        }
    }

    config_defaults(&cfg);
    if (config_load(&cfg, cfgpath) < 0)
        logmsg("config %s not readable, using defaults", cfgpath);
    if (!cfg.nbands)
        config_default_bands(&cfg);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    for (int rx = 0; rx < NRX; rx++) {
        rx_view[rx] = -1;
        rx_client[rx] = -1;
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        cl[i].fd = -1;
        cl[i].view = -1;
        cl[i].rx = -1;
    }

    if (hw_init(cfg.clock) < 0)
        return 1;
    /* The FPGA only streams once every receiver has been configured (as r2t2srv did). */
    for (int rx = 0; rx < NRX; rx++) {
        hw_set_freq(rx, cfg.nbands ? cfg.bands[0].center : 7000000);
        hw_set_input(rx, 1);
    }
    for (int adc = 0; adc < 2; adc++) {
        hw_set_gain(adc, cfg.gain[adc]);
        hw_set_att(adc, cfg.att[adc]);
    }
    if (dsp_global_init() < 0) {
        logmsg("fftw init failed");
        return 1;
    }

    /* views 0..nbands-1 are the configured bands, the rest free centres */
    int navg = (int)lrint((double)FS_WIDE / WF_FFT / cfg.wf_fps);
    nviews = cfg.nbands + NRX;
    for (int i = 0; i < nviews; i++) {
        view_t *v = &views[i];
        v->rx = -1;
        v->band = i < cfg.nbands ? i : -1;
        v->used = i < cfg.nbands;
        if (i < cfg.nbands) {
            v->center = cfg.bands[i].center;
            v->input = cfg.bands[i].input;
        }
        if (wf_init(&v->wf, navg) < 0) {
            logmsg("out of memory");
            return 1;
        }
    }

    int sfd = hw_open_stream(cfg.ifname);
    int lfd = net_listen(cfg.port);
    if (sfd < 0 || lfd < 0)
        return 1;
    logmsg("r2t2sdr %s listening on port %d, %d bands, www %s", VERSION, cfg.port, cfg.nbands, cfg.www);

    struct pollfd pfd[2 + MAX_CLIENTS];
    int map[2 + MAX_CLIENTS];
    time_t last_status = time(NULL), last_stats = last_status;
    unsigned long npkt = 0;

    while (running) {
        int n = 0;
        pfd[n].fd = lfd; pfd[n].events = POLLIN; n++;
        pfd[n].fd = sfd; pfd[n].events = POLLIN; n++;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (cl[i].fd < 0)
                continue;
            pfd[n].fd = cl[i].fd;
            pfd[n].events = POLLIN | (cl[i].outlen ? POLLOUT : 0);
            map[n] = i;
            n++;
        }

        if (poll(pfd, n, 100) < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        npkt += hw_stream_read(on_packet);

        if (pfd[0].revents & POLLIN) {
            char ip[48];
            int fd;
            while ((fd = net_accept(lfd, ip, sizeof(ip))) >= 0) {
                int slot = -1, used = 0;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (cl[i].fd >= 0)
                        used++;
                    else if (slot < 0)
                        slot = i;
                }
                if (slot < 0 || used >= cfg.max_clients) {
                    close(fd);
                    continue;
                }
                client_t *c = &cl[slot];
                memset(c, 0, sizeof(*c));
                c->fd = fd;
                c->view = -1;
                c->rx = -1;
                c->last_rx = now_s();
                snprintf(c->ip, sizeof(c->ip), "%s", ip);
            }
        }

        for (int k = 2; k < n; k++) {
            client_t *c = &cl[map[k]];
            if (c->fd != pfd[k].fd)
                continue;
            if (pfd[k].revents & (POLLERR | POLLNVAL)) {
                client_close(c);
                continue;
            }
            if (pfd[k].revents & (POLLIN | POLLHUP)) {
                c->last_rx = now_s();
                if (net_read(c, cfg.www, &callbacks) < 0) {
                    client_close(c);
                    continue;
                }
            }
        }

        /* browsers ping every 10 s; drop peers that vanished without closing */
        long mono = now_s();
        for (int i = 0; i < MAX_CLIENTS; i++) {
            client_t *c = &cl[i];
            if (c->fd >= 0 && mono - c->last_rx > (c->ws ? CLIENT_TIMEOUT : 10)) {
                logmsg("client %s timed out", c->ip);
                client_close(c);
            }
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            client_t *c = &cl[i];
            if (c->fd < 0)
                continue;
            if (net_flush(c) < 0 || (c->closing && !c->outlen))
                client_close(c);
        }

        time_t now = time(NULL);
        if (now - last_status >= 5) {
            last_status = now;
            broadcast_status();
        }
        if (now - last_stats >= 60) {
            unsigned drops = hw_stream_drops(sfd);
            if (verbose || drops)
                logmsg("stream: %lu frames/min, %u kernel drops", npkt, drops);
            npkt = 0;
            last_stats = now;
        }
    }

    logmsg("shutting down");
    for (int i = 0; i < MAX_CLIENTS; i++)
        client_close(&cl[i]);
    close(lfd);
    close(sfd);
    return 0;
}
