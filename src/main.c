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

#include <strings.h>
#include <termios.h>

#include "antennas.h"
#include "auth.h"
#include "bookmarks.h"
#include "common.h"
#include "config.h"
#include "dsp.h"
#include "hw.h"
#include "net.h"

#define MAX_VIEWS      (MAX_BANDS + NRX)
#define CLIENT_TIMEOUT 30
#define VERSION        "0.3.0"
#define LOGIN_FAILS    5      /* failed logins per address ... */
#define LOGIN_LOCK_S   60     /* ... before it is locked for this long */

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
    utf8_trim(dst);
}

static const char *json_find(const char *s, const char *key)
{
    char k[48];
    snprintf(k, sizeof(k), "\"%s\"", key);
    /* the same text can appear as a value first; a key is followed by ':' */
    for (const char *p = strstr(s, k); p; p = strstr(p + 1, k)) {
        const char *q = p + strlen(k);
        while (*q == ' ')
            q++;
        if (*q != ':')
            continue;
        q++;
        while (*q == ' ')
            q++;
        return q;
    }
    return NULL;
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

static void put_utf8(char *out, size_t n, size_t *i, unsigned cp)
{
    char b[4];
    size_t k;
    if (cp < 0x80)        { b[0] = cp; k = 1; }
    else if (cp < 0x800)  { b[0] = 0xc0 | cp >> 6; b[1] = 0x80 | (cp & 63); k = 2; }
    else if (cp < 0x10000){ b[0] = 0xe0 | cp >> 12; b[1] = 0x80 | ((cp >> 6) & 63); b[2] = 0x80 | (cp & 63); k = 3; }
    else                  { b[0] = 0xf0 | cp >> 18; b[1] = 0x80 | ((cp >> 12) & 63);
                            b[2] = 0x80 | ((cp >> 6) & 63); b[3] = 0x80 | (cp & 63); k = 4; }
    if (*i + k < n) {
        memcpy(out + *i, b, k);
        *i += k;
    }
}

/* string value with JSON escapes; control characters become spaces */
static int json_str(const char *s, const char *key, char *out, size_t n)
{
    const char *p = json_find(s, key);
    if (!p || *p != '"')
        return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"') {
        unsigned c = (unsigned char)*p++;
        if (c == '\\') {
            c = (unsigned char)*p++;
            if (c == 'u') {
                unsigned cp = 0;
                if (sscanf(p, "%4x", &cp) != 1)
                    break;
                p += 4;
                if (cp >= 0xd800 && cp < 0xdc00 && p[0] == '\\' && p[1] == 'u') {
                    unsigned lo = 0;
                    if (sscanf(p + 2, "%4x", &lo) == 1 && lo >= 0xdc00 && lo < 0xe000) {
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                        p += 6;
                    }
                }
                put_utf8(out, n, &i, cp < 0x20 ? ' ' : cp);
                continue;
            }
            if (!c)
                break;
            if (c == 'n' || c == 't' || c == 'r' || c == 'b' || c == 'f')
                c = ' ';
        } else if (c < 0x20) {
            c = ' ';
        }
        if (i + 1 < n)
            out[i++] = (char)c;
    }
    out[i] = 0;
    utf8_trim(out);
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
    char t[200], cs[64], loc[200], lc[32], a1[110], a2[110];
    json_escape(t, sizeof(t), cfg.title);
    json_escape(cs, sizeof(cs), cfg.callsign);
    json_escape(loc, sizeof(loc), cfg.location);
    json_escape(lc, sizeof(lc), cfg.locator);
    json_escape(a1, sizeof(a1), ant_get(1)->name);
    json_escape(a2, sizeof(a2), ant_get(2)->name);
    int o = snprintf(b, n,
                     "{\"type\":\"config\",\"version\":\"%s\",\"title\":\"%s\",\"callsign\":\"%s\","
                     "\"location\":\"%s\",\"locator\":\"%s\",\"span\":%d,\"bins\":%d,\"audioRate\":%d,"
                     "\"receivers\":%d,\"maxFreq\":%.0f,\"access\":\"%s\",\"chat\":\"%s\","
                     "\"antennas\":[\"%s\",\"%s\"],\"bands\":[",
                     VERSION, t, cs, loc, lc, FS_WIDE, WF_BINS, AUDIO_RATE, NRX, cfg.clock / 2,
                     cfg.access ? "login" : "open", cfg.chat == 0 ? "off" : cfg.chat == 1 ? "login" : "all",
                     a1, a2);
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

/*
 * Antenna input for a frequency: the ranges set under Verwaltung > Antennen,
 * if any; otherwise the input of the configured band covering f; otherwise
 * the default antenna.
 */
static int input_for(double f)
{
    if (ant_configured())
        return ant_for(f);
    for (int b = 0; b < cfg.nbands; b++)
        if (fabs(f - cfg.bands[b].center) < FS_WIDE / 2)
            return cfg.bands[b].input;
    return ant_default();
}

static void send_view(client_t *c)
{
    view_t *v = &views[c->view];
    char m[300], an[110];
    json_escape(an, sizeof(an), ant_get(v->input)->name);
    snprintf(m, sizeof(m), "{\"type\":\"view\",\"id\":%d,\"band\":%d,\"center\":%.0f,\"span\":%d,"
             "\"input\":%d,\"antenna\":\"%s\"}",
             c->view, v->band, v->center, FS_WIDE, v->input, an);
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
            v->input = input_for(v->center);
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
        v->rx = -1;
        v->viewers = 0;
        v->used = 1;
        view_join(c, i);
        return;
    }
    send_error(c, "Zu viele Ansichten offen");
}

/* ---------------------------------------------------------------- listeners */

static void send_audio_state(client_t *c)
{
    char m[200], an[110] = "";
    if (c->listening && c->input)
        json_escape(an, sizeof(an), ant_get(c->input)->name);
    snprintf(m, sizeof(m), "{\"type\":\"audio\",\"on\":%s,\"input\":%d,\"antenna\":\"%s\"}",
             c->listening ? "true" : "false", c->listening ? c->input : 0, an);
    send_text(c, m);
}

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
    int in = input_for(c->freq);
    if (in != c->input) {
        c->input = in;
        hw_set_input(c->rx, in);
        if (c->listening)
            send_audio_state(c);
    }
    hw_set_freq(c->rx, f);
    demod_set(&c->dm, c->mode, lo, hi);
}

/* after antenna changes: put every active receiver on its input again */
static void reapply_inputs(void)
{
    for (int i = 0; i < nviews; i++) {
        view_t *v = &views[i];
        if (v->rx < 0)
            continue;
        v->input = input_for(v->center);
        hw_set_input(v->rx, v->input);
        for (int k = 0; k < MAX_CLIENTS; k++)
            if (cl[k].fd >= 0 && cl[k].ws && cl[k].view == i)
                send_view(&cl[k]);
    }
    for (int k = 0; k < MAX_CLIENTS; k++)
        if (cl[k].fd >= 0 && cl[k].rx >= 0) {
            cl[k].input = 0;
            apply_tune(&cl[k]);
        }
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
    if (cfg.access && c->role < ROLE_USER) {
        send_error(c, "Hören ist nur für angemeldete Nutzer freigegeben. Bitte melde dich an.");
        send_audio_state(c);
        return;
    }
    if (c->rx < 0) {
        int rx = rx_alloc();
        if (rx < 0) {
            send_error(c, "Alle Empfänger belegt, bitte später erneut versuchen");
            send_audio_state(c);
            return;
        }
        c->rx = rx;
        rx_client[rx] = (int)(c - cl);
        c->input = 0;
        demod_init(&c->dm);
        c->dm.sql = c->sql;
        apply_tune(c);
    }
    c->listening = 1;
    send_audio_state(c);
    broadcast_status();
}

/* ---------------------------------------------------------------- accounts */

typedef struct {
    char ip[48];
    int fails;
    long until;
} lock_t;
static lock_t locks[32];

static lock_t *lock_for(const char *ip, int create)
{
    lock_t *free_slot = NULL, *oldest = &locks[0];
    for (int i = 0; i < 32; i++) {
        if (!strcmp(locks[i].ip, ip))
            return &locks[i];
        if (!locks[i].ip[0] && !free_slot)
            free_slot = &locks[i];
        if (locks[i].until < oldest->until)
            oldest = &locks[i];
    }
    if (!create)
        return NULL;
    lock_t *l = free_slot ? free_slot : oldest;
    memset(l, 0, sizeof(*l));
    snprintf(l->ip, sizeof(l->ip), "%s", ip);
    return l;
}

static void send_login(client_t *c, const char *token)
{
    char m[300], u[64];
    if (!c->role) {
        send_text(c, "{\"type\":\"logout\"}");
        return;
    }
    json_escape(u, sizeof(u), c->user);
    snprintf(m, sizeof(m), "{\"type\":\"login\",\"user\":\"%s\",\"role\":\"%s\"%s%s%s}",
             u, role_name(c->role), token ? ",\"token\":\"" : "", token ? token : "", token ? "\"" : "");
    send_text(c, m);
}

/* the role of an account changed or it was deleted: update its open connections */
static void refresh_user(const char *name)
{
    const user_t *u = auth_user_find(name);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_t *c = &cl[i];
        if (c->fd < 0 || !c->ws || strcasecmp(c->user, name))
            continue;
        c->role = u ? u->role : ROLE_NONE;
        if (!u)
            c->user[0] = 0;
        send_login(c, NULL);
        if (cfg.access && c->role < ROLE_USER && c->listening) {
            listener_stop(c);
            send_audio_state(c);
        }
    }
}

static void send_bookmarks(client_t *c)
{
    static char b[24000];
    bm_json(b, sizeof(b));
    if (c)
        send_text(c, b);
    else
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (cl[i].fd >= 0 && cl[i].ws)
                send_text(&cl[i], b);
}

static void send_users(client_t *c)
{
    static char b[8192];
    int o = snprintf(b, sizeof(b), "{\"type\":\"users\",\"list\":[");
    for (int i = 0; i < auth_user_count(); i++) {
        const user_t *u = auth_user_at(i);
        int online = 0;
        for (int k = 0; k < MAX_CLIENTS; k++)
            online += cl[k].fd >= 0 && cl[k].ws && !strcasecmp(cl[k].user, u->name);
        o += snprintf(b + o, sizeof(b) - o, "%s{\"name\":\"%s\",\"role\":\"%s\",\"online\":%d}",
                      i ? "," : "", u->name, role_name(u->role), online);
    }
    snprintf(b + o, sizeof(b) - o, "]}");
    send_text(c, b);
}

static void send_antennas(client_t *c)
{
    char b[2048];
    ant_json(b, sizeof(b), cfg.gain, cfg.att);
    send_text(c, b);
}

static void broadcast_config(void)
{
    static char b[8192];
    config_json(b, sizeof(b));
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (cl[i].fd >= 0 && cl[i].ws)
            send_text(&cl[i], b);
}

static void send_ok(client_t *c, const char *what)
{
    char m[120];
    snprintf(m, sizeof(m), "{\"type\":\"ok\",\"what\":\"%s\"}", what);
    send_text(c, m);
}

/* settings travel through a key = value file: no line breaks, no '#' (comment) */
static void station_str(char *dst, size_t n, const char *src)
{
    size_t o = 0;
    for (; *src && o + 1 < n; src++)
        if (*src != '#' && *src != '\n' && *src != '\r')
            dst[o++] = *src;
    dst[o] = 0;
    utf8_trim(dst);
}

static int require(client_t *c, int role)
{
    if (c->role >= role)
        return 1;
    send_error(c, c->role ? "Dafür fehlt deinem Konto die Berechtigung."
                          : "Dafür musst du angemeldet sein.");
    return 0;
}

/* nonce of an open challenge for this name, used once, valid 60 s */
static int take_nonce(client_t *c, const char *name, char nonce[33])
{
    int ok = c->nonce[0] && !strcasecmp(c->nonce_user, name) && now_s() - c->nonce_t <= 60;
    snprintf(nonce, 33, "%s", c->nonce);
    c->nonce[0] = 0;
    return ok;
}

/* password set by the browser as salt + PBKDF2 hash (the password itself never arrives) */
static int read_hash(const char *txt, char salt[40], char hash[80], int *iter)
{
    double it = 0;
    if (!json_str(txt, "salt", salt, 40) || !json_str(txt, "hash", hash, 80))
        return 0;
    json_num(txt, "iter", &it);
    *iter = (int)it;
    return 1;
}

static int account_cmd(client_t *c, const char *cmd, const char *txt)
{
    char name[64], s[200], proof[80], nonce[33];
    double v;

    if (!strcmp(cmd, "challenge")) {
        char salt[33], purpose[12] = "login";
        int iter;
        if (!json_str(txt, "user", name, sizeof(name)) || !name[0]) {
            send_error(c, "Bitte einen Namen eingeben.");
            return 1;
        }
        json_str(txt, "purpose", purpose, sizeof(purpose));
        auth_challenge(name, salt, &iter);
        auth_random_hex(c->nonce, 16);
        snprintf(c->nonce_user, sizeof(c->nonce_user), "%s", name);
        c->nonce_t = now_s();
        char m[300], u[64];
        json_escape(u, sizeof(u), name);
        snprintf(m, sizeof(m), "{\"type\":\"challenge\",\"purpose\":\"%s\",\"user\":\"%s\",\"salt\":\"%s\","
                 "\"iter\":%d,\"nonce\":\"%s\"}", strcmp(purpose, "passwd") ? "login" : "passwd",
                 u, salt, iter, c->nonce);
        send_text(c, m);
        return 1;
    }
    if (!strcmp(cmd, "login")) {
        lock_t *l = lock_for(c->ip, 0);
        long now = now_s();
        if (l && l->until > now) {
            snprintf(s, sizeof(s), "Zu viele Fehlversuche. Bitte warte %ld Sekunden.", l->until - now);
            send_error(c, s);
            return 1;
        }
        char token[65];
        if (!json_str(txt, "user", name, sizeof(name)) || !json_str(txt, "proof", proof, sizeof(proof)) ||
            !take_nonce(c, name, nonce) || auth_verify(name, nonce, proof) < 0) {
            l = lock_for(c->ip, 1);
            if (++l->fails >= LOGIN_FAILS) {
                l->until = now + LOGIN_LOCK_S;
                l->fails = 0;
            }
            logmsg("login failed for '%s' from %s", name, c->ip);
            send_error(c, "Anmeldung fehlgeschlagen: Name oder Passwort stimmt nicht.");
            return 1;
        }
        if (l)
            l->fails = 0;
        const user_t *u = auth_user_find(name);
        snprintf(c->user, sizeof(c->user), "%s", u->name);
        c->role = u->role;
        if (auth_session_new(c->user, token) < 0) {
            send_error(c, "Anmeldung konnte nicht gespeichert werden.");
            return 1;
        }
        logmsg("login %s (%s) from %s", c->user, role_name(c->role), c->ip);
        send_login(c, token);
        return 1;
    }
    if (!strcmp(cmd, "auth")) {
        char token[80];
        if (json_str(txt, "token", token, sizeof(token)) &&
            auth_session_get(token, name, sizeof(name)) == 0) {
            const user_t *u = auth_user_find(name);
            snprintf(c->user, sizeof(c->user), "%s", u->name);
            c->role = u->role;
        } else {
            c->user[0] = 0;
            c->role = ROLE_NONE;
        }
        send_login(c, NULL);
        return 1;
    }
    if (!strcmp(cmd, "logout")) {
        char token[80];
        if (json_str(txt, "token", token, sizeof(token)))
            auth_session_drop(token);
        c->user[0] = 0;
        c->role = ROLE_NONE;
        if (cfg.access && c->listening) {
            listener_stop(c);
            send_audio_state(c);
        }
        send_login(c, NULL);
        return 1;
    }
    if (!strcmp(cmd, "passwd")) {
        char salt[40], hash[80];
        int iter;
        if (!require(c, ROLE_USER))
            return 1;
        if (!json_str(txt, "proof", proof, sizeof(proof)) || !take_nonce(c, c->user, nonce) ||
            auth_verify(c->user, nonce, proof) < 0) {
            send_error(c, "Das bisherige Passwort stimmt nicht.");
            return 1;
        }
        if (!read_hash(txt, salt, hash, &iter) || auth_user_set_hash(c->user, c->role, salt, hash, iter) < 0) {
            send_error(c, "Passwort konnte nicht gespeichert werden.");
            return 1;
        }
        auth_session_drop_user(c->user);   /* other devices log in again */
        char token[65];
        if (auth_session_new(c->user, token) == 0)
            send_login(c, token);
        send_ok(c, "passwd");
        return 1;
    }

    /* ---- bookmarks (role lesezeichen or admin) */
    if (!strcmp(cmd, "bm_set")) {
        char mode[8] = "usb";
        double id = 0, f;
        if (!require(c, ROLE_EDITOR))
            return 1;
        json_num(txt, "id", &id);
        json_str(txt, "mode", mode, sizeof(mode));
        if (!json_num(txt, "freq", &f) || f <= 0 || f > cfg.clock / 2 || mode_parse(mode) < 0 ||
            !json_str(txt, "name", s, sizeof(s)) || !s[0]) {
            send_error(c, "Lesezeichen braucht Name, Frequenz und Betriebsart.");
            return 1;
        }
        if (bm_set((int)id, f, mode_name(mode_parse(mode)), s) < 0)
            send_error(c, "Lesezeichen konnte nicht gespeichert werden.");
        else
            send_bookmarks(NULL);
        return 1;
    }
    if (!strcmp(cmd, "bm_del")) {
        if (!require(c, ROLE_EDITOR))
            return 1;
        if (!json_num(txt, "id", &v) || bm_del((int)v) < 0)
            send_error(c, "Lesezeichen nicht gefunden.");
        else
            send_bookmarks(NULL);
        return 1;
    }

    /* ---- administration */
    if (!strcmp(cmd, "users")) {
        if (require(c, ROLE_ADMIN))
            send_users(c);
        return 1;
    }
    if (!strcmp(cmd, "user_set")) {
        char role[16];
        if (!require(c, ROLE_ADMIN))
            return 1;
        if (!json_str(txt, "name", name, sizeof(name)) || !auth_valid_name(name)) {
            send_error(c, "Name: 2 bis 24 Zeichen, nur Buchstaben, Ziffern, Punkt, Minus und Unterstrich.");
            return 1;
        }
        int r = json_str(txt, "role", role, sizeof(role)) ? role_parse(role) : ROLE_NONE;
        if (r == ROLE_NONE) {
            send_error(c, "Unbekannte Rolle.");
            return 1;
        }
        const user_t *u = auth_user_find(name);
        char salt[40], hash[80];
        int iter = 0, newpw = read_hash(txt, salt, hash, &iter);
        if (!u && !newpw) {
            send_error(c, "Ein neues Konto braucht ein Passwort mit mindestens 8 Zeichen.");
            return 1;
        }
        if (u && u->role == ROLE_ADMIN && r != ROLE_ADMIN && auth_admin_count() <= 1) {
            send_error(c, "Das ist der letzte Admin. Lege erst einen weiteren an.");
            return 1;
        }
        int rc = newpw ? auth_user_set_hash(name, r, salt, hash, iter) : auth_user_set(name, r, NULL);
        if (rc < 0) {
            send_error(c, "Konto konnte nicht gespeichert werden.");
            return 1;
        }
        if (newpw && u)
            auth_session_drop_user(name);   /* new password: log out everywhere */
        logmsg("account %s set to %s by %s", name, role_name(r), c->user);
        refresh_user(name);
        send_users(c);
        return 1;
    }
    if (!strcmp(cmd, "user_del")) {
        if (!require(c, ROLE_ADMIN))
            return 1;
        if (!json_str(txt, "name", name, sizeof(name)) || !auth_user_find(name)) {
            send_error(c, "Konto nicht gefunden.");
            return 1;
        }
        if (!strcasecmp(name, c->user)) {
            send_error(c, "Das eigene Konto kannst du nicht löschen.");
            return 1;
        }
        if (auth_user_find(name)->role == ROLE_ADMIN && auth_admin_count() <= 1) {
            send_error(c, "Das ist der letzte Admin.");
            return 1;
        }
        auth_user_del(name);
        logmsg("account %s deleted by %s", name, c->user);
        refresh_user(name);
        send_users(c);
        return 1;
    }
    if (!strcmp(cmd, "station_set")) {
        char acc[16];
        if (!require(c, ROLE_ADMIN))
            return 1;
        if (json_str(txt, "title", s, sizeof(s)))    station_str(cfg.title, sizeof(cfg.title), s);
        if (json_str(txt, "callsign", s, sizeof(s))) station_str(cfg.callsign, sizeof(cfg.callsign), s);
        if (json_str(txt, "location", s, sizeof(s))) station_str(cfg.location, sizeof(cfg.location), s);
        if (json_str(txt, "locator", s, sizeof(s)))  station_str(cfg.locator, sizeof(cfg.locator), s);
        if (json_str(txt, "access", acc, sizeof(acc)))
            cfg.access = !strcmp(acc, "login");
        if (json_str(txt, "chat", acc, sizeof(acc)))
            cfg.chat = !strcmp(acc, "off") ? 0 : !strcmp(acc, "login") ? 1 : 2;
        if (config_save_station(&cfg) < 0)
            send_error(c, "Einstellungen konnten nicht gespeichert werden.");
        else
            send_ok(c, "station");
        broadcast_config();
        if (cfg.access)
            for (int i = 0; i < MAX_CLIENTS; i++)
                if (cl[i].fd >= 0 && cl[i].listening && cl[i].role < ROLE_USER) {
                    listener_stop(&cl[i]);
                    send_audio_state(&cl[i]);
                }
        broadcast_status();
        return 1;
    }
    if (!strcmp(cmd, "antennas")) {
        if (require(c, ROLE_ADMIN))
            send_antennas(c);
        return 1;
    }
    if (!strcmp(cmd, "ant_set")) {
        char ranges[512];
        double in, gain, att, def;
        if (!require(c, ROLE_ADMIN))
            return 1;
        if (!json_num(txt, "input", &in) || in < 1 || in > NANT) {
            send_error(c, "Unbekannter Antenneneingang.");
            return 1;
        }
        if (!json_str(txt, "name", s, sizeof(s)))
            s[0] = 0;
        if (!json_str(txt, "ranges", ranges, sizeof(ranges)))
            ranges[0] = 0;
        if (ant_set((int)in, s, ranges) < 0) {
            send_error(c, "Bereiche bitte in kHz als von-bis, mit Komma getrennt, z. B. 3500-3800, 7000-7200.");
            return 1;
        }
        int a = (int)in - 1;
        if (json_num(txt, "gain", &gain) && gain >= -9 && gain <= 32 && (int)gain != cfg.gain[a]) {
            cfg.gain[a] = (int)gain;
            hw_set_gain(a, cfg.gain[a]);
        }
        if (json_num(txt, "att", &att) && att >= 0 && att <= 31 && (int)att != cfg.att[a]) {
            cfg.att[a] = (int)att;
            hw_set_att(a, cfg.att[a]);
        }
        if (json_num(txt, "default", &def) && def)
            ant_set_default((int)in);
        config_save_station(&cfg);
        reapply_inputs();
        broadcast_config();
        send_antennas(c);
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- chat */

#define CHAT_KEEP     30     /* messages a newcomer sees */
#define CHAT_PER_MIN  10

typedef struct {
    char who[64];
    char ts[24];
    char text[600];      /* JSON-escaped */
    int guest;
} chat_msg_t;
static chat_msg_t chat_log[CHAT_KEEP];
static int chat_count, chat_head;

static void send_chat_msg(client_t *to, const chat_msg_t *m)
{
    char b[900], who[140];
    json_escape(who, sizeof(who), m->who);
    snprintf(b, sizeof(b), "{\"type\":\"chat\",\"who\":\"%s\",\"guest\":%s,\"ts\":\"%s\",\"text\":\"%s\"}",
             who, m->guest ? "true" : "false", m->ts, m->text);
    send_text(to, b);
}

static void send_chat_history(client_t *c)
{
    for (int i = 0; i < chat_count; i++)
        send_chat_msg(c, &chat_log[(chat_head - chat_count + i + CHAT_KEEP) % CHAT_KEEP]);
}

static void chat_cmd(client_t *c, const char *txt)
{
    char text[400], name[64];
    if (cfg.chat == 0 || (cfg.chat == 1 && c->role < ROLE_USER)) {
        send_error(c, cfg.chat ? "Chatten können hier nur angemeldete Nutzer." : "Der Chat ist ausgeschaltet.");
        return;
    }
    long now = now_s();
    if (now - c->chat_t >= 60) {
        c->chat_t = now;
        c->chat_n = 0;
    }
    if (++c->chat_n > CHAT_PER_MIN) {
        send_error(c, "Bitte etwas langsamer schreiben.");
        return;
    }
    if (!json_str(txt, "text", text, sizeof(text)))
        return;
    char *t = text;
    while (*t == ' ')
        t++;
    if (!*t)
        return;

    chat_msg_t *m = &chat_log[chat_head];
    memset(m, 0, sizeof(*m));
    if (c->role) {
        snprintf(m->who, sizeof(m->who), "%s", c->user);
    } else {
        /* guests choose a name, it is marked as such */
        if (!json_str(txt, "name", name, sizeof(name)) || !name[0])
            snprintf(name, sizeof(name), "Gast %d", (int)(c - cl) + 1);
        name[24] = 0;
        utf8_trim(name);
        snprintf(m->who, sizeof(m->who), "%s", name);
        m->guest = 1;
    }
    time_t wall = time(NULL);
    strftime(m->ts, sizeof(m->ts), "%Y-%m-%dT%H:%M:%SZ", gmtime(&wall));
    json_escape(m->text, sizeof(m->text), t);
    chat_head = (chat_head + 1) % CHAT_KEEP;
    if (chat_count < CHAT_KEEP)
        chat_count++;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (cl[i].fd >= 0 && cl[i].ws)
            send_chat_msg(&cl[i], m);
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
    send_bookmarks(c);
    send_chat_history(c);
    broadcast_status();
}

static void on_text(client_t *c, char *txt, size_t len)
{
    (void)len;
    char cmd[16];
    double v;
    if (!json_str(txt, "cmd", cmd, sizeof(cmd)))
        return;

    if (account_cmd(c, cmd, txt))
        return;
    if (!strcmp(cmd, "chat")) {
        chat_cmd(c, txt);
        return;
    }
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
    fprintf(stderr,
            "usage: %s [-c config] [-v]\n"
            "       %s [-c config] --user NAME [--role admin|lesezeichen|nutzer]\n"
            "           create an account or set its password (asks for it)\n"
            "       %s [-c config] --users\n"
            "           list the accounts\n",
            prog, prog, prog);
}

/* read a password from the terminal without echo, or a line from stdin */
static int read_password(const char *prompt, char *buf, size_t n)
{
    struct termios old, quiet;
    int tty = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old) == 0;
    fputs(prompt, stderr);
    if (tty) {
        quiet = old;
        quiet.c_lflag &= ~ECHO;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
    }
    char *r = fgets(buf, (int)n, stdin);
    if (tty) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
        fputc('\n', stderr);
    }
    if (!r)
        return -1;
    buf[strcspn(buf, "\r\n")] = 0;
    return 0;
}

static int cli_user(const char *name, const char *role_s)
{
    int role = role_parse(role_s);
    if (!auth_valid_name(name) || role == ROLE_NONE) {
        fprintf(stderr, "invalid name or role\n");
        return 1;
    }
    char p1[160], p2[160];
    if (read_password("Passwort: ", p1, sizeof(p1)) < 0)
        return 1;
    if (isatty(STDIN_FILENO) && (read_password("Wiederholen: ", p2, sizeof(p2)) < 0 || strcmp(p1, p2))) {
        fprintf(stderr, "Passwörter stimmen nicht überein\n");
        return 1;
    }
    if (strlen(p1) < 8) {
        fprintf(stderr, "Passwort braucht mindestens 8 Zeichen\n");
        return 1;
    }
    if (auth_user_set(name, role, p1) < 0) {
        fprintf(stderr, "konnte %s nicht speichern (Rechte auf %s?)\n", name, cfg.state_dir);
        return 1;
    }
    auth_session_drop_user(name);
    fprintf(stderr, "Konto %s (%s) gespeichert\n", name, role_name(role));
    return 0;
}

int main(int argc, char **argv)
{
    const char *cfgpath = "/etc/r2t2sdr.conf";
    const char *new_user = NULL, *new_role = "admin";
    int list_users = 0;
    static const struct option longopts[] = {
        { "user", required_argument, NULL, 'U' },
        { "role", required_argument, NULL, 'R' },
        { "users", no_argument, NULL, 'L' },
        { NULL, 0, NULL, 0 },
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "c:vh", longopts, NULL)) != -1) {
        switch (opt) {
        case 'c': cfgpath = optarg; break;
        case 'v': verbose = 1; break;
        case 'U': new_user = optarg; break;
        case 'R': new_role = optarg; break;
        case 'L': list_users = 1; break;
        default: usage(argv[0]); return 1;
        }
    }

    config_defaults(&cfg);
    if (config_load(&cfg, cfgpath) < 0)
        logmsg("config %s not readable, using defaults", cfgpath);
    if (!cfg.nbands)
        config_default_bands(&cfg);
    config_load_station(&cfg);
    auth_init(cfg.state_dir);

    if (new_user)
        return cli_user(new_user, new_role);
    if (list_users) {
        for (int i = 0; i < auth_user_count(); i++)
            printf("%-24s %s\n", auth_user_at(i)->name, role_name(auth_user_at(i)->role));
        return 0;
    }

    bm_init(cfg.state_dir);
    ant_init(cfg.state_dir);
    if (!auth_admin_count())
        logmsg("no admin account yet: create one with  r2t2sdr --user NAME --role admin");

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
