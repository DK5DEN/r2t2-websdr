/*
 * r2t2sdr - multi-user WebSDR for the DARC R2T2 receiver.
 *
 * Waterfall views are fed by FPGA receivers (192 kS/s wide streams); a band
 * is widened over several receivers side by side while receivers are free.
 * Listeners inside a view are cut out of its wide stream by the CPU (DDC) on
 * a second thread and cost no receiver; outside every view a listener gets a
 * receiver of its own (16 kS/s narrow stream). Receivers go by priority:
 * station > logged-in user > guest.
 *
 * A view is either one of the configured bands or a free centre frequency
 * (used when the page follows an external rig). Clients on the same view share
 * its receivers.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
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

#define MAX_SEG        4        /* receivers side by side in one waterfall */
#define SEG_USABLE     85000.0  /* usable half width of a 192 kHz wide stream */
#define DDC_REACH      88000.0  /* listener frequency may be this far from a segment centre */
#define RX_RESERVE     1        /* receivers kept free when widening bands */

typedef struct {
    int used;      /* configured band, or free view in use */
    int band;      /* configured band index, -1 for a free view */
    double center; /* configured or requested centre */
    int input;     /* antenna input of segment 0 */
    int viewers;
    int nseg;      /* receivers feeding the waterfall, 0 = inactive */
    int seg_rx[MAX_SEG];
    double seg_center[MAX_SEG];
    double show_lo, show_hi;   /* range the waterfall shows */
    wf_t wf[MAX_SEG];
} view_t;

static config_t cfg;
static client_t cl[MAX_CLIENTS];
static view_t views[MAX_VIEWS];
static int nviews;
static int rx_view[NRX];
static int rx_seg[NRX];
static int rx_client[NRX];
static volatile sig_atomic_t running = 1;
static int verbose;
/*
 * Listener threads: listeners fed from the wide stream (DDC + demod + ADPCM,
 * ~9.5 % of a core each) run on NWORK threads, one per core; client slot i
 * belongs to thread i % NWORK. The main loop hands each thread the samples of
 * every segment its listeners need (worker_t.ring) and sends the audio
 * packets the threads leave in each client's aq. Listener state (dview, dseg,
 * ddc, dm, listening, client slots) changes only under dsp_lock(), which takes
 * every thread's mutex.
 */
#define NWORK    2
#define BLK_RING 1024
typedef struct {
    int16_t view, seg, n;
    float iq[2 * 128];
} blk_t;
typedef struct {
    blk_t ring[BLK_RING];
    unsigned head, tail, drops;
    pthread_mutex_t lock;
    int id;
} worker_t;
static worker_t work[NWORK];

static void dsp_lock(void)
{
    for (int w = 0; w < NWORK; w++)
        pthread_mutex_lock(&work[w].lock);
}

static void dsp_unlock(void)
{
    for (int w = NWORK - 1; w >= 0; w--)
        pthread_mutex_unlock(&work[w].lock);
}

static int no_ddc;      /* --no-ddc: every listener gets its own receiver (for comparisons) */

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/*
 * Activity log (Verwaltung > Online, switched on there): one line per event,
 * "YYYY-MM-DD HH:MM:SS<TAB>who<TAB>address<TAB>event", UTC, in
 * state_dir/activity.log; at 1 MB it becomes activity.log.1.
 */
#define ACT_LOG_MAX (1 << 20)

static void act_path(char *p, size_t n, int old)
{
    snprintf(p, n, "%s/activity.log%s", cfg.state_dir, old ? ".1" : "");
}

static void act_log(const client_t *c, const char *fmt, ...)
{
    if (!cfg.log)
        return;
    char path[300], ev[300], when[32];
    act_path(path, sizeof(path), 0);
    struct stat sb;
    if (stat(path, &sb) == 0 && sb.st_size > ACT_LOG_MAX) {
        char old[300];
        act_path(old, sizeof(old), 1);
        rename(path, old);
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ev, sizeof(ev), fmt, ap);
    va_end(ap);
    time_t t = time(NULL);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", gmtime(&t));
    FILE *f = fopen(path, "a");
    if (!f)
        return;
    const char *who = !c ? "" : c->user[0] ? c->user : c->gname[0] ? c->gname : "Gast";
    fprintf(f, "%s\t%s%s\t%s\t%s\n", when, who, c && !c->user[0] && c->gname[0] ? " (Gast)" : "", c ? c->ip : "", ev);
    fclose(f);
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

/*
 * JSON string starting at p (on the opening quote) into out, escapes resolved,
 * control characters as spaces. Returns the position after the closing quote,
 * NULL if p is not a string.
 */
static const char *json_read_str(const char *p, char *out, size_t n)
{
    if (!p || *p != '"')
        return NULL;
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
    return *p == '"' ? p + 1 : p;
}

static int json_str(const char *s, const char *key, char *out, size_t n)
{
    return json_read_str(json_find(s, key), out, n) != NULL;
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

static const char *mode_upper(int m)
{
    static const char *n[] = { "USB", "LSB", "CW", "AM", "FM" };
    return m >= 0 && m <= M_FM ? n[m] : "?";
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

static int rx_free_count(void);
static int view_holders(int id, int *prio);
static int view_listeners(int id);

/* counts plus every active waterfall, so visitors see where they can listen without a receiver */
static int status_json(char *b, size_t n)
{
    int listeners, users = count_users(&listeners);
    int o = snprintf(b, n, "{\"type\":\"status\",\"users\":%d,\"listeners\":%d,\"free\":%d,\"total\":%d,"
                     "\"names\":[", users, listeners, rx_free_count(), NRX);
    /* who is online, for everybody: accounts and guests who gave a chat name once each, other guests counted */
    int guests = 0, nn = 0;
    for (int i = 0; i < MAX_CLIENTS && o < (int)n - 400; i++) {
        const client_t *k = &cl[i];
        if (k->fd < 0 || !k->ws)
            continue;
        const char *nm = k->user[0] ? k->user : k->gname;
        if (!nm[0]) {
            guests++;
            continue;
        }
        int dup = 0;
        for (int j = 0; j < i && !dup; j++)
            dup = cl[j].fd >= 0 && cl[j].ws && !strcmp(cl[j].user[0] ? cl[j].user : cl[j].gname, nm)
                  && !cl[j].user[0] == !k->user[0];
        if (dup)
            continue;
        char e[80];
        json_escape(e, sizeof(e), nm);
        o += snprintf(b + o, n - o, "%s{\"name\":\"%s\",\"guest\":%s}", nn++ ? "," : "", e,
                      k->user[0] ? "false" : "true");
    }
    o += snprintf(b + o, n - o, "],\"guests\":%d,\"active\":[", guests);
    int first = 1;
    for (int i = 0; i < nviews && o < (int)n - 120; i++) {
        const view_t *v = &views[i];
        if (!v->nseg)
            continue;
        o += snprintf(b + o, n - o, "%s{\"view\":%d,\"band\":%d,\"center\":%.0f,\"lo\":%.0f,\"hi\":%.0f,"
                      "\"segments\":%d,\"viewers\":%d,\"listeners\":%d}", first ? "" : ",", i, v->band,
                      v->center, v->nseg == 1 ? v->seg_center[0] - SEG_USABLE : v->show_lo,
                      v->nseg == 1 ? v->seg_center[0] + SEG_USABLE : v->show_hi,
                      v->nseg, v->viewers, view_listeners(i));
        first = 0;
    }
    o += snprintf(b + o, n - o, "]}");
    return o;
}

static void broadcast_status(void)
{
    static char b[4096];
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
        o += snprintf(b + o, n - o, "%s{\"id\":%d,\"name\":\"%s\",\"center\":%.0f,\"mode\":\"%s\","
                      "\"lo\":%.0f,\"hi\":%.0f}",
                      i ? "," : "", i, nm, cfg.bands[i].center, cfg.bands[i].mode, cfg.bands[i].lo, cfg.bands[i].hi);
    }
    o += snprintf(b + o, n - o, "]}");
    return o;
}

/* ---------------------------------------------------------------- views */

/*
 * A view is one waterfall: a configured band or a free centre frequency. It
 * is fed by 1..MAX_SEG receivers (segments) side by side. Listeners inside a
 * segment are cut out of its 192 kS/s wide stream (DDC) and cost no receiver;
 * outside every segment a listener gets a receiver of its own (narrow stream).
 *
 * Receivers are handed out by priority: free ones first, then extra segments
 * of widened bands, then whatever the lowest-priority client holds, if that
 * is below the asking client (guest < logged in < station).
 */

static int prio_of(const client_t *c)
{
    return c->role == ROLE_STATION ? 3 : c->role >= ROLE_USER ? 2 : 1;
}

/* clients keeping a view alive (viewers and listeners on its wide stream), highest priority among them */
static int view_holders(int id, int *prio)
{
    int n = 0, p = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        const client_t *c = &cl[i];
        if (c->fd < 0 || !c->ws)
            continue;
        if (c->view == id || (c->listening && c->dview == id)) {
            n++;
            if (prio_of(c) > p)
                p = prio_of(c);
        }
    }
    if (prio)
        *prio = p;
    return n;
}

/* last activity among a view's holders: a shared view is as active as its most active user */
static long view_activity(int id)
{
    long t = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        const client_t *c = &cl[i];
        if (c->fd >= 0 && c->ws && (c->view == id || (c->listening && c->dview == id)) && c->last_act > t)
            t = c->last_act;
    }
    return t;
}

static int view_listeners(int id)
{
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (cl[i].fd >= 0 && cl[i].listening && cl[i].dview == id)
            n++;
    return n;
}

/* segments a band needs to be shown in full (capped at MAX_SEG) */
static int seg_needed(const view_t *v)
{
    if (v->band < 0)
        return 1;
    const band_cfg_t *b = &cfg.bands[v->band];
    if (b->hi <= b->lo)
        return 1;
    int n = (int)ceil((b->hi - b->lo) / (2 * SEG_USABLE));
    return n < 1 ? 1 : n > MAX_SEG ? MAX_SEG : n;
}

/* centres and shown range for n segments: one segment keeps the plain centre,
   more are spread evenly over the band around its configured centre */
static void seg_layout(view_t *v, int n)
{
    if (n <= 1 || v->band < 0) {
        v->seg_center[0] = v->center;
        v->show_lo = v->center - FS_WIDE / 2;
        v->show_hi = v->center + FS_WIDE / 2;
        return;
    }
    const band_cfg_t *b = &cfg.bands[v->band];
    double w = b->hi - b->lo, cover = n * 2 * SEG_USABLE;
    if (cover > w)
        cover = w;
    double start = v->center - cover / 2;
    if (start < b->lo)
        start = b->lo;
    if (start + cover > b->hi)
        start = b->hi - cover;
    double step = cover / n;
    for (int s = 0; s < n; s++)
        v->seg_center[s] = start + step * (s + 0.5);
    v->show_lo = start;
    v->show_hi = start + cover;
}

static void send_view(client_t *c)
{
    view_t *v = &views[c->view];
    char m[900], an[110];
    json_escape(an, sizeof(an), ant_get(v->input)->name);
    int o = snprintf(m, sizeof(m), "{\"type\":\"view\",\"id\":%d,\"band\":%d,\"center\":%.0f,\"span\":%d,"
                     "\"lo\":%.0f,\"hi\":%.0f,\"input\":%d,\"antenna\":\"%s\",\"segments\":[",
                     c->view, v->band, v->center, FS_WIDE, v->show_lo, v->show_hi, v->input, an);
    double half = (v->show_hi - v->show_lo) / v->nseg / 2;
    for (int s = 0; s < v->nseg; s++)
        o += snprintf(m + o, sizeof(m) - o, "%s{\"center\":%.0f,\"lo\":%.0f,\"hi\":%.0f}", s ? "," : "",
                      v->seg_center[s], v->seg_center[s] - half, v->seg_center[s] + half);
    snprintf(m + o, sizeof(m) - o, "]}");
    send_text(c, m);
}

static void send_noview(client_t *c, const char *why)
{
    char m[400], w[300];
    json_escape(w, sizeof(w), why);
    snprintf(m, sizeof(m), "{\"type\":\"view\",\"id\":-1,\"reason\":\"%s\"}", w);
    send_text(c, m);
}

static void send_view_all(int id)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (cl[i].fd >= 0 && cl[i].ws && cl[i].view == id)
            send_view(&cl[i]);
}

static int input_for(double f);
static void listener_route(client_t *c, int may_alloc);
static void listener_stop(client_t *c);
static void send_audio_state(client_t *c);
static void reroute_all(void);

/* put the view on n segments; seg_rx[0..n-1] must be assigned, receivers beyond n are freed */
static void view_apply(int id, int n)
{
    view_t *v = &views[id];
    for (int s = n; s < v->nseg; s++)
        if (v->seg_rx[s] >= 0) {
            rx_view[v->seg_rx[s]] = -1;
            v->seg_rx[s] = -1;
        }
    v->nseg = n;
    seg_layout(v, n);
    v->input = input_for(v->seg_center[0]);
    for (int s = 0; s < n; s++) {
        int rx = v->seg_rx[s];
        rx_view[rx] = id;
        rx_seg[rx] = s;
        hw_set_input(rx, input_for(v->seg_center[s]));
        hw_set_freq(rx, v->seg_center[s]);
        wf_reset(&v->wf[s]);
    }
}

/* drop all receivers of a view; its viewers lose the waterfall (with a reason if given) */
static void view_release(int id, const char *why)
{
    view_t *v = &views[id];
    for (int s = 0; s < v->nseg; s++)
        if (v->seg_rx[s] >= 0) {
            rx_view[v->seg_rx[s]] = -1;
            v->seg_rx[s] = -1;
        }
    v->nseg = 0;
    v->viewers = 0;
    if (v->band < 0)
        v->used = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_t *c = &cl[i];
        if (c->fd < 0)
            continue;
        if (c->view == id) {
            c->view = -1;
            if (why && c->ws) {
                send_noview(c, why);
                act_log(c, "Wasserfall verdrängt");
            }
        }
        if (c->dview == id) {
            c->dview = c->dseg = -1;
            if (c->listening)
                listener_route(c, 0);
        }
    }
}

static int rx_free_count(void)
{
    int n = 0;
    for (int rx = 0; rx < NRX; rx++)
        if (rx_view[rx] < 0 && rx_client[rx] < 0)
            n++;
    return n;
}

static int rx_take(int prio);

/* a view nobody watches or listens to any more gives its receivers back */
static void view_check(int id)
{
    if (id >= 0 && views[id].nseg && !view_holders(id, NULL))
        view_release(id, NULL);
}

/* add segments while receivers are free (beyond the reserve); 1 if the view changed */
static int view_widen(int id, int max_add)
{
    view_t *v = &views[id];
    int n = v->nseg, need = seg_needed(v);
    while (n < need && max_add-- > 0 && rx_free_count() > RX_RESERVE) {
        int rx = rx_take(0);
        if (rx < 0)
            break;
        v->seg_rx[n++] = rx;
        rx_view[rx] = id;   /* reserve it until view_apply */
    }
    if (n == v->nseg)
        return 0;
    view_apply(id, n);
    return 1;
}

/*
 * A receiver for a client of priority prio (0 = band widening), -1 if none.
 * May shrink a widened band or take a receiver from a lower-priority client.
 */
static int rx_take(int prio)
{
    for (int rx = 0; rx < NRX; rx++)
        if (rx_view[rx] < 0 && rx_client[rx] < 0)
            return rx;
    if (prio <= 0)
        return -1;

    /* extra segments come back first, from the widest view */
    int best = -1;
    for (int i = 0; i < nviews; i++)
        if (views[i].nseg > 1 && (best < 0 || views[i].nseg > views[best].nseg))
            best = i;
    if (best >= 0) {
        view_t *v = &views[best];
        int rx = v->seg_rx[v->nseg - 1];
        view_apply(best, v->nseg - 1);
        send_view_all(best);
        reroute_all();
        if (rx_view[rx] < 0 && rx_client[rx] < 0)
            return rx;
    }

    /* holder below prio: lowest priority first (guests), among those the one
       inactive for the longest time; own listener receivers and views alike */
    int vrx = -1, bp = prio;
    long bact = 0;
    for (int rx = 0; rx < NRX; rx++) {
        int p;
        long act;
        if (rx_client[rx] >= 0) {
            p = prio_of(&cl[rx_client[rx]]);
            act = cl[rx_client[rx]].last_act;
        } else if (rx_view[rx] >= 0) {
            view_holders(rx_view[rx], &p);
            act = view_activity(rx_view[rx]);
        } else {
            continue;
        }
        if (p < bp || (p == bp && vrx >= 0 && act < bact)) {
            bp = p;
            bact = act;
            vrx = rx;
        }
    }
    const char *why = prio == 3 ? "Der Empfänger wird für die Remote-Station gebraucht."
                                : "Der Empfänger wurde für einen angemeldeten Nutzer freigegeben.";
    if (vrx >= 0 && rx_client[vrx] >= 0) {
        client_t *c = &cl[rx_client[vrx]];
        act_log(c, "eigener Empfänger verdrängt (%.4f MHz)", c->freq / 1e6);
        listener_stop(c);
        send_error(c, why);
        send_audio_state(c);
        return vrx;
    }
    if (vrx >= 0) {
        char msg[300];
        snprintf(msg, sizeof(msg), "%s Hören geht weiter auf allen Bändern, die gerade empfangen werden.", why);
        view_release(rx_view[vrx], msg);
        if (rx_view[vrx] < 0 && rx_client[vrx] < 0)
            return vrx;
    }
    return -1;
}

static void view_leave(client_t *c)
{
    if (c->view < 0)
        return;
    int id = c->view;
    view_t *v = &views[id];
    if (v->viewers > 0)
        v->viewers--;
    c->view = -1;
    view_check(id);
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
        if (fabs(f - cfg.bands[b].center) < FS_WIDE / 2 ||
            (cfg.bands[b].lo < cfg.bands[b].hi && f >= cfg.bands[b].lo && f <= cfg.bands[b].hi))
            return cfg.bands[b].input;
    return ant_default();
}

static void view_join(client_t *c, int id)
{
    view_t *v = &views[id];
    if (c->view != id) {
        /* leave first so a view seen only by this client frees its receivers */
        view_leave(c);
        if (v->nseg == 0) {
            int rx = rx_take(prio_of(c));
            if (rx < 0) {
                if (v->band < 0 && !view_holders(id, NULL))
                    v->used = 0;
                send_noview(c, "Alle Empfänger belegt. Hören geht weiter auf allen Bändern, die gerade empfangen werden.");
                broadcast_status();
                return;
            }
            v->seg_rx[0] = rx;
            view_apply(id, 1);
            view_widen(id, MAX_SEG);
        }
        v->used = 1;
        v->viewers++;
        c->view = id;
        reroute_all();
        if (v->band >= 0)
            act_log(c, "Wasserfall %s", cfg.bands[v->band].name);
        else
            act_log(c, "Wasserfall %.3f MHz", v->center / 1e6);
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
        if (views[i].used && views[i].nseg && fabs(views[i].center - center) < 1) {
            view_join(c, i);
            return;
        }
    for (int i = cfg.nbands; i < nviews; i++) {
        view_t *v = &views[i];
        if (v->used || i == c->view)
            continue;
        v->center = center;
        v->nseg = 0;
        v->viewers = 0;
        v->used = 1;
        view_join(c, i);
        return;
    }
    send_noview(c, "Alle Empfänger belegt. Hören geht weiter auf allen Bändern, die gerade empfangen werden.");
}

/* once a second: widen one viewed band while receivers are free (most viewers first) */
static void widen_tick(void)
{
    if (rx_free_count() <= RX_RESERVE)
        return;
    int best = -1, bestn = 0;
    for (int i = 0; i < cfg.nbands; i++) {
        view_t *v = &views[i];
        if (!v->nseg || v->nseg >= seg_needed(v))
            continue;
        int n = view_holders(i, NULL);
        if (n > bestn) { bestn = n; best = i; }
    }
    if (best < 0)
        return;
    if (!view_widen(best, 1))
        return;
    send_view_all(best);
    reroute_all();
    broadcast_status();
}

/* ---------------------------------------------------------------- listeners */

static void send_audio_state(client_t *c)
{
    char m[220], an[110] = "";
    if (c->listening && c->input)
        json_escape(an, sizeof(an), ant_get(c->input)->name);
    snprintf(m, sizeof(m), "{\"type\":\"audio\",\"on\":%s,\"input\":%d,\"antenna\":\"%s\",\"own\":%s}",
             c->listening ? "true" : "false", c->listening ? c->input : 0, an,
             c->rx >= 0 ? "true" : "false");
    send_text(c, m);
}

/* segment whose wide stream reaches f with the listener passband, nearest centre */
static int seg_for(double f, int *vi, int *si)
{
    double best = DDC_REACH + 1;
    for (int i = 0; i < nviews; i++)
        for (int s = 0; s < views[i].nseg; s++) {
            double d = fabs(f - views[i].seg_center[s]);
            if (d < best) { best = d; *vi = i; *si = s; }
        }
    return best <= DDC_REACH;
}

static double listener_nco(const client_t *c)
{
    /* CW: the signal lands at +CW_PITCH, the real part gives the beat note */
    return c->mode == M_CW ? c->freq - CW_PITCH : c->freq;
}

/*
 * Feed a listener from a segment if one covers its frequency, else from a
 * receiver of its own (allocated only if may_alloc; otherwise it stops).
 */
static void listener_route(client_t *c, int may_alloc)
{
    if (!c->listening)
        return;
    float lo = c->lo, hi = c->hi;
    double f = listener_nco(c);
    if (c->mode == M_CW) {
        lo += CW_PITCH;
        hi += CW_PITCH;
    }
    int vi = -1, si = -1, in;
    /* a chosen antenna: the waterfall's stream only if that receiver is on it */
    if (!no_ddc && seg_for(f, &vi, &si) &&
        (!c->ant_pref || input_for(views[vi].seg_center[si]) == c->ant_pref)) {
        if (c->rx >= 0) {
            rx_client[c->rx] = -1;
            c->rx = -1;
        }
        if (c->dview != vi || c->dseg != si) {
            int old = c->dview;
            ddc_init(&c->ddc);
            c->dview = vi;
            c->dseg = si;
            if (old != vi)
                view_check(old);
        }
        ddc_set(&c->ddc, f - views[vi].seg_center[si]);
        in = input_for(views[vi].seg_center[si]);
    } else {
        int old = c->dview;
        c->dview = c->dseg = -1;
        view_check(old);
        if (c->rx < 0) {
            int rx = may_alloc ? rx_take(prio_of(c)) : -1;
            if (rx < 0) {
                listener_stop(c);
                send_error(c, "Alle Empfänger belegt. Hören geht ohne eigenen Empfänger auf allen Bändern, "
                              "die gerade empfangen werden.");
                send_audio_state(c);
                broadcast_status();
                return;
            }
            c->rx = rx;
            rx_client[rx] = (int)(c - cl);
        }
        in = c->ant_pref ? c->ant_pref : input_for(c->freq);
        hw_set_input(c->rx, in);
        hw_set_freq(c->rx, f);
    }
    demod_set(&c->dm, c->mode, lo, hi);
    if (in != c->input) {
        c->input = in;
        send_audio_state(c);
    }
}

/* after views or segments changed: move listeners to the best source, free own receivers */
static void reroute_all(void)
{
    for (int k = 0; k < MAX_CLIENTS; k++) {
        client_t *c = &cl[k];
        if (c->fd < 0 || !c->listening)
            continue;
        int had = c->rx >= 0, vi = -1, si = -1;
        if (c->dview >= 0 && c->dseg >= views[c->dview].nseg)
            c->dview = c->dseg = -1;
        if (c->dview < 0 || (seg_for(listener_nco(c), &vi, &si) && (vi != c->dview || si != c->dseg)) ||
            (c->dview >= 0 && fabs(listener_nco(c) - views[c->dview].seg_center[c->dseg]) > DDC_REACH))
            listener_route(c, 0);
        if (c->listening && had != (c->rx >= 0))
            send_audio_state(c);
    }
}

static void apply_tune(client_t *c)
{
    int was_own = c->rx >= 0;
    listener_route(c, 1);
    if (c->listening && was_own != (c->rx >= 0)) {
        send_audio_state(c);
        broadcast_status();
    }
}

/* after antenna changes: put every active receiver on its input again */
static void reapply_inputs(void)
{
    for (int i = 0; i < nviews; i++) {
        view_t *v = &views[i];
        if (!v->nseg)
            continue;
        v->input = input_for(v->seg_center[0]);
        for (int s = 0; s < v->nseg; s++)
            hw_set_input(v->seg_rx[s], input_for(v->seg_center[s]));
        send_view_all(i);
    }
    for (int k = 0; k < MAX_CLIENTS; k++)
        if (cl[k].fd >= 0 && cl[k].listening) {
            cl[k].input = 0;
            listener_route(&cl[k], 0);
        }
}

static void listener_stop(client_t *c)
{
    if (c->rx >= 0) {
        rx_client[c->rx] = -1;
        c->rx = -1;
    }
    int dv = c->dview;
    c->dview = c->dseg = -1;
    c->listening = 0;
    /* a view kept only by this listener's audio goes away */
    view_check(dv);
}

/* CPU load of the listeners in units of an SSB listener: the steep filter for
   250 Hz and narrower (641 taps) costs ~17 % of a core instead of ~9.5 % */
static int listener_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (cl[i].fd >= 0 && cl[i].listening)
            n += cl[i].dm.nt >= DM_NT ? 2 : 1;
    return n;
}

static void listener_start(client_t *c)
{
    if (cfg.access && c->role < ROLE_USER) {
        send_error(c, "Hören ist nur für angemeldete Nutzer freigegeben. Bitte melde dich an.");
        send_audio_state(c);
        return;
    }
    if (!c->listening && listener_count() >= cfg.max_listeners) {
        /* CPU guard: a higher-priority listener bumps a lower one */
        client_t *low = NULL;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            client_t *o = &cl[i];
            if (o->fd < 0 || !o->listening || prio_of(o) >= prio_of(c))
                continue;
            /* lowest priority first, then the one inactive for the longest time */
            if (!low || prio_of(o) < prio_of(low) || (prio_of(o) == prio_of(low) && o->last_act < low->last_act))
                low = o;
        }
        if (!low) {
            send_error(c, "Alle Hörplätze belegt, bitte später erneut versuchen.");
            send_audio_state(c);
            return;
        }
        listener_stop(low);
        send_error(low, prio_of(c) == 3 ? "Der Hörplatz wird für die Remote-Station gebraucht."
                                        : "Der Hörplatz wurde für einen angemeldeten Nutzer freigegeben.");
        send_audio_state(low);
    }
    if (!c->listening) {
        c->listening = 1;
        c->input = 0;
        c->dview = c->dseg = -1;
        demod_init(&c->dm);
        c->dm.sql = c->sql;
    }
    listener_route(c, 1);
    if (!c->listening)
        return;
    act_log(c, "hört %.4f MHz %s%s", c->freq / 1e6, mode_upper(c->mode), c->rx >= 0 ? " (eigener Empfänger)" : "");
    c->log_freq = c->freq;
    c->log_t = now_s();
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
        broadcast_status();
        return;
    }
    json_escape(u, sizeof(u), c->user);
    snprintf(m, sizeof(m), "{\"type\":\"login\",\"user\":\"%s\",\"role\":\"%s\"%s%s%s}",
             u, role_name(c->role), token ? ",\"token\":\"" : "", token ? token : "", token ? "\"" : "");
    send_text(c, m);
    broadcast_status();
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
    /* up to MAX_BOOKMARKS entries of at most ~200 bytes */
    static char b[MAX_BOOKMARKS * 200 + 64];
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

/* newest ACT_SEND lines of the activity log, newest first */
#define ACT_SEND 500
static void send_act_log(client_t *c)
{
    static char raw[96 << 10], b[160 << 10];
    /* the end of activity.log, and if that is short the end of activity.log.1 before it */
    char p0[300], p1[300];
    act_path(p0, sizeof(p0), 0);
    act_path(p1, sizeof(p1), 1);
    struct stat s0, s1;
    long sz0 = stat(p0, &s0) == 0 ? (long)s0.st_size : 0, sz1 = stat(p1, &s1) == 0 ? (long)s1.st_size : 0;
    long cap = (long)sizeof(raw) - 1;
    long take0 = sz0 < cap ? sz0 : cap, take1 = sz1 < cap - take0 ? sz1 : cap - take0;
    int cut = take0 < sz0 || take1 < sz1;   /* first line in the window may be partial */
    size_t len = 0;
    const char *paths[2] = { p1, p0 };
    long sizes[2] = { sz1, sz0 }, takes[2] = { take1, take0 };
    for (int k = 0; k < 2; k++) {
        if (!takes[k])
            continue;
        FILE *f = fopen(paths[k], "r");
        if (!f)
            continue;
        fseek(f, sizes[k] - takes[k], SEEK_SET);
        len += fread(raw + len, 1, (size_t)takes[k], f);
        fclose(f);
    }
    raw[len] = 0;
    int o = snprintf(b, sizeof(b), "{\"type\":\"log\",\"on\":%s,\"lines\":[", cfg.log ? "true" : "false");
    int count = 0;
    char *end = raw + len;
    while (end > raw && count < ACT_SEND && o < (int)sizeof(b) - 1200) {
        char *e = end;
        if (e > raw && e[-1] == '\n')
            e--;
        char *s = e;
        while (s > raw && s[-1] != '\n')
            s--;
        end = s;
        if (s == e)
            continue;
        if (s == raw && cut)
            break; /* partial line at the start of the window */
        char line[600];
        size_t ln = (size_t)(e - s) < sizeof(line) - 1 ? (size_t)(e - s) : sizeof(line) - 1;
        memcpy(line, s, ln);
        line[ln] = 0;
        char *f[4] = { line, "", "", "" };
        for (int k = 1; k < 4; k++) {
            char *t = strchr(f[k - 1], '\t');
            if (!t)
                break;
            *t = 0;
            f[k] = t + 1;
        }
        char e0[40], e1[100], e2[100], e3[700];
        json_escape(e0, sizeof(e0), f[0]);
        json_escape(e1, sizeof(e1), f[1]);
        json_escape(e2, sizeof(e2), f[2]);
        json_escape(e3, sizeof(e3), f[3]);
        o += snprintf(b + o, sizeof(b) - o, "%s[\"%s\",\"%s\",\"%s\",\"%s\"]", count++ ? "," : "", e0, e1, e2, e3);
    }
    snprintf(b + o, sizeof(b) - o, "]}");
    send_text(c, b);
}

/* who is connected and what they do, for admins */
static void send_online(client_t *c)
{
    static char b[16384];
    long now = now_s();
    int o = snprintf(b, sizeof(b), "{\"type\":\"online\",\"list\":[");
    int first = 1;
    for (int i = 0; i < MAX_CLIENTS && o < (int)sizeof(b) - 600; i++) {
        const client_t *k = &cl[i];
        if (k->fd < 0 || !k->ws)
            continue;
        char user[64], gname[64], ip[64], view[96] = "";
        json_escape(user, sizeof(user), k->user);
        json_escape(gname, sizeof(gname), k->gname);
        json_escape(ip, sizeof(ip), k->ip);
        if (k->view >= 0) {
            const view_t *v = &views[k->view];
            if (v->band >= 0)
                json_escape(view, sizeof(view), cfg.bands[v->band].name);
            else
                snprintf(view, sizeof(view), "%.3f-%.3f MHz", v->show_lo / 1e6, v->show_hi / 1e6);
        }
        o += snprintf(b + o, sizeof(b) - o,
                      "%s{\"slot\":%d,\"user\":\"%s\",\"role\":\"%s\",\"guest\":\"%s\",\"ip\":\"%s\","
                      "\"since\":%ld,\"idle\":%ld,\"view\":\"%s\",\"segments\":%d,\"zoom\":%s,"
                      "\"listening\":%s,\"freq\":%.0f,\"mode\":\"%s\",\"own\":%s,\"self\":%s}",
                      first ? "" : ",", i, user, k->user[0] ? role_name(k->role) : "", gname, ip,
                      now - k->since, now - k->last_act, view, k->view >= 0 ? views[k->view].nseg : 0,
                      k->zhi > k->zlo ? "true" : "false", k->listening ? "true" : "false", k->freq,
                      mode_name(k->mode), k->rx >= 0 ? "true" : "false", k == c ? "true" : "false");
        first = 0;
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
        act_log(c, "angemeldet (%s)", role_name(c->role));
        send_login(c, token);
        return 1;
    }
    if (!strcmp(cmd, "auth")) {
        char token[80];
        if (json_str(txt, "token", token, sizeof(token)) &&
            auth_session_get(token, name, sizeof(name)) == 0) {
            const user_t *u = auth_user_find(name);
            if (strcasecmp(c->user, u->name))
                act_log(c, "angemeldet als %s (gespeicherte Sitzung)", u->name);
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
        act_log(c, "abgemeldet");
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

    /*
     * Bulk import in parts that fit a message: items = [[freq, "mode", "name"], ...].
     * "replace" on the first part empties the list, "done" on the last one sends
     * the new list to everybody. The browser maps OpenWebRX modes before.
     */
    if (!strcmp(cmd, "bm_import")) {
        double replace = 0, done = 0;
        int added = 0, dup = 0, full = 0, bad = 0;
        if (!require(c, ROLE_EDITOR))
            return 1;
        json_num(txt, "replace", &replace);
        json_num(txt, "done", &done);
        const char *p = json_find(txt, "items");
        if (!p || *p != '[') {
            send_error(c, "Import: keine Lesezeichen gefunden.");
            return 1;
        }
        if (replace)
            bm_clear();
        p++;
        while (*p) {
            while (*p == ' ' || *p == ',')
                p++;
            if (*p != '[')
                break;
            p++;
            char *e;
            double f = strtod(p, &e);
            char mode[16], nm[200];
            p = e;
            while (*p == ' ' || *p == ',')
                p++;
            p = json_read_str(p, mode, sizeof(mode));
            if (p) {
                while (*p == ' ' || *p == ',')
                    p++;
                p = json_read_str(p, nm, sizeof(nm));
            }
            if (!p)
                break;
            while (*p && *p != ']')
                p++;
            if (*p == ']')
                p++;
            int m = mode_parse(mode);
            if (f <= 0 || f > cfg.clock / 2 || !nm[0]) {
                bad++;
                continue;
            }
            int r = bm_add(f, mode_name(m < 0 ? M_USB : m), nm);
            if (r > 0) added++;
            else if (r == 0) dup++;
            else full++;
        }
        bm_save();
        if (done)
            send_bookmarks(NULL);
        char m[200];
        snprintf(m, sizeof(m), "{\"type\":\"ok\",\"what\":\"bm_import\",\"added\":%d,\"duplicates\":%d,"
                 "\"full\":%d,\"invalid\":%d,\"done\":%s}", added, dup, full, bad, done ? "true" : "false");
        send_text(c, m);
        return 1;
    }

    /* ---- administration */
    if (!strcmp(cmd, "users")) {
        if (require(c, ROLE_ADMIN))
            send_users(c);
        return 1;
    }
    if (!strcmp(cmd, "online")) {
        if (require(c, ROLE_ADMIN))
            send_online(c);
        return 1;
    }
    if (!strcmp(cmd, "log")) {
        if (require(c, ROLE_ADMIN))
            send_act_log(c);
        return 1;
    }
    if (!strcmp(cmd, "log_set")) {
        char on[8];
        if (!require(c, ROLE_ADMIN))
            return 1;
        if (json_str(txt, "on", on, sizeof(on))) {
            int was = cfg.log;
            if (was && strcmp(on, "on"))
                act_log(c, "Protokoll ausgeschaltet");
            cfg.log = !strcmp(on, "on");
            if (!was && cfg.log)
                act_log(c, "Protokoll eingeschaltet");
            config_save_station(&cfg);
        }
        if (json_str(txt, "clear", on, sizeof(on)) && !strcmp(on, "yes")) {
            char p[300];
            act_path(p, sizeof(p), 0);
            unlink(p);
            act_path(p, sizeof(p), 1);
            unlink(p);
            act_log(c, "Protokoll gelöscht");
        }
        send_act_log(c);
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
        if (strcmp(c->gname, name)) {
            act_log(c, "nennt sich im Chat \"%s\"", name);
            snprintf(c->gname, sizeof(c->gname), "%s", name);
            broadcast_status();
        }
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
    c->last_act = now_s();
    c->since = c->last_act;
    c->view = -1;
    c->rx = -1;
    c->dview = c->dseg = -1;
    c->listening = 0;
    c->freq = cfg.nbands ? cfg.bands[0].center : 7100000;
    c->mode = M_LSB;
    c->lo = -2700;
    c->hi = -300;
    c->sql = -999;
    logmsg("client %s connected", c->ip);
    act_log(c, "verbunden");
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
    if (strcmp(cmd, "ping"))
        c->last_act = now_s();

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
        if (c->listening && fabs(c->freq - c->log_freq) >= 3000 && now_s() - c->log_t >= 15) {
            act_log(c, "hört %.4f MHz %s", c->freq / 1e6, mode_upper(c->mode));
            c->log_freq = c->freq;
            c->log_t = now_s();
        }
    } else if (!strcmp(cmd, "start")) {
        listener_start(c);
    } else if (!strcmp(cmd, "stop")) {
        if (c->listening)
            act_log(c, "hört nicht mehr: %.4f MHz %s", c->freq / 1e6, mode_upper(c->mode));
        listener_stop(c);
        send_audio_state(c);
        broadcast_status();
    } else if (!strcmp(cmd, "antenna")) {
        /* 0 = by frequency, 1..NANT = this input for the listener's own audio */
        if (json_num(txt, "input", &v) && v >= 0 && v <= NANT) {
            c->ant_pref = (int)v;
            if (c->listening) {
                int was_own = c->rx >= 0;
                listener_route(c, 1);
                if (c->listening) {
                    act_log(c, "Antenne %s", c->ant_pref ? ant_get(c->ant_pref)->name : "automatisch");
                    send_audio_state(c);
                    if (was_own != (c->rx >= 0))
                        broadcast_status();
                }
            }
        }
    } else if (!strcmp(cmd, "zoom")) {
        /* the part of the view this client shows; without lo/hi the whole view */
        double lo, hi;
        if (json_num(txt, "lo", &lo) && json_num(txt, "hi", &hi) && hi - lo >= 2000 && lo > 0) {
            c->zlo = lo;
            c->zhi = hi;
        } else {
            c->zlo = c->zhi = 0;
        }
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
    dsp_lock();
    int was_ws = c->ws;
    if (was_ws) {
        logmsg("client %s disconnected", c->ip);
        long d = now_s() - c->since;
        if (c->listening)
            act_log(c, "hört nicht mehr: %.4f MHz %s", c->freq / 1e6, mode_upper(c->mode));
        act_log(c, "getrennt nach %ld min %ld s", d / 60, d % 60);
    }
    view_leave(c);
    listener_stop(c);
    close(c->fd);
    free(c->out);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    c->view = -1;
    c->rx = -1;
    c->dview = c->dseg = -1;
    dsp_unlock();
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
    static uint8_t frame[3 + WF_BINS];
    static uint8_t zf[7 + ZOOM_ROW];
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
            int v = rx_view[rx], sg = rx_seg[rx];
            /* frame: 1, view, segment, bins */
            if (wf_push(&views[v].wf[sg], iq[rx], per, frame + 3)) {
                frame[0] = 1;
                frame[1] = (uint8_t)v;
                frame[2] = (uint8_t)sg;
                const view_t *vw = &views[v];
                double half = (vw->show_hi - vw->show_lo) / vw->nseg / 2;
                double slo = vw->seg_center[sg] - half, shi = vw->seg_center[sg] + half;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    client_t *c = &cl[i];
                    if (c->fd < 0 || !c->ws || c->view != v)
                        continue;
                    if (c->zhi <= c->zlo) {
                        net_ws_send(c, 2, frame, sizeof(frame), 1);
                        continue;
                    }
                    /* zoomed: 3, view, segment, x0:uint16, n:uint16, n points of a
                       ZOOM_ROW line over [zlo, zhi] taken from this segment */
                    double df = (c->zhi - c->zlo) / ZOOM_ROW;
                    double a = slo > c->zlo ? slo : c->zlo, b = shi < c->zhi ? shi : c->zhi;
                    if (b <= a)
                        continue;
                    int x0 = (int)ceil((a - c->zlo) / df - 0.5), x1 = (int)floor((b - c->zlo) / df - 0.5) + 1;
                    if (x0 < 0) x0 = 0;
                    if (x1 > ZOOM_ROW) x1 = ZOOM_ROW;
                    int n = x1 - x0;
                    if (n <= 0)
                        continue;
                    wf_zoom(&vw->wf[sg], c->zlo + (x0 + 0.5) * df - vw->seg_center[sg], df, n, zf + 7);
                    zf[0] = 3;
                    zf[1] = (uint8_t)v;
                    zf[2] = (uint8_t)sg;
                    zf[3] = (uint8_t)(x0 >> 8);
                    zf[4] = (uint8_t)x0;
                    zf[5] = (uint8_t)(n >> 8);
                    zf[6] = (uint8_t)n;
                    net_ws_send(c, 2, zf, 7 + n, 1);
                }
            }
            /* listeners cut out of this segment's wide stream: to their listener thread */
            int want[NWORK] = { 0 };
            for (int i = 0; i < MAX_CLIENTS; i++)
                if (cl[i].fd >= 0 && cl[i].listening && cl[i].dview == v && cl[i].dseg == sg)
                    want[i % NWORK] = 1;
            for (int k = 0; k < NWORK; k++) {
                if (!want[k])
                    continue;
                worker_t *wk = &work[k];
                unsigned h = wk->head;
                if (h - __atomic_load_n(&wk->tail, __ATOMIC_ACQUIRE) >= BLK_RING) {
                    wk->drops++;
                    continue;
                }
                blk_t *b = &wk->ring[h % BLK_RING];
                b->view = (int16_t)v;
                b->seg = (int16_t)sg;
                b->n = (int16_t)per;
                memcpy(b->iq, iq[rx], 2 * per * sizeof(float));
                __atomic_store_n(&wk->head, h + 1, __ATOMIC_RELEASE);
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

/* ---------------------------------------------------------------- listener thread */

static void *listener_thread(void *arg)
{
    worker_t *wk = arg;
    float nb[2 * 32];
    uint8_t pkt[AQ_SIZE];
    for (;;) {
        unsigned t = wk->tail;
        if (t == __atomic_load_n(&wk->head, __ATOMIC_ACQUIRE)) {
            usleep(2000);
            continue;
        }
        const blk_t *b = &wk->ring[t % BLK_RING];
        pthread_mutex_lock(&wk->lock);
        for (int i = wk->id; i < MAX_CLIENTS; i += NWORK) {
            client_t *c = &cl[i];
            if (c->fd < 0 || !c->listening || c->dview != b->view || c->dseg != b->seg)
                continue;
            int m = ddc_process(&c->ddc, b->iq, b->n, nb, 32);
            demod_process(&c->dm, nb, m);
            int n;
            while ((n = demod_packet(&c->dm, pkt)) > 0) {
                unsigned h = c->aq_head;
                if (h - __atomic_load_n(&c->aq_tail, __ATOMIC_ACQUIRE) >= AQ_LEN)
                    continue;   /* main loop behind: drop */
                memcpy(c->aq[h % AQ_LEN], pkt, n);
                c->aqn[h % AQ_LEN] = (uint16_t)n;
                __atomic_store_n(&c->aq_head, h + 1, __ATOMIC_RELEASE);
            }
        }
        pthread_mutex_unlock(&wk->lock);
        __atomic_store_n(&wk->tail, t + 1, __ATOMIC_RELEASE);
    }
    return NULL;
}

/* send what the listener thread produced */
static void drain_audio(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_t *c = &cl[i];
        if (c->fd < 0)
            continue;
        unsigned h = __atomic_load_n(&c->aq_head, __ATOMIC_ACQUIRE);
        while (c->aq_tail != h) {
            unsigned t = c->aq_tail;
            net_ws_send(c, 2, c->aq[t % AQ_LEN], c->aqn[t % AQ_LEN], 1);
            __atomic_store_n(&c->aq_tail, t + 1, __ATOMIC_RELEASE);
        }
    }
}

static int start_listener_threads(void)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    for (int w = 0; w < NWORK; w++) {
        work[w].id = w;
        pthread_mutex_init(&work[w].lock, &a);
        pthread_t th;
        if (pthread_create(&th, NULL, listener_thread, &work[w]))
            return -1;
        /* thread 0 gets the second core to itself; thread 1 shares the first
           with the main loop, which needs ~45 % of it with 8 waterfalls */
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(w == 0 ? 1 : 0, &set);
        pthread_setaffinity_np(th, sizeof(set), &set);
    }
    return 0;
}

/* ---------------------------------------------------------------- main */

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [-c config] [-v]\n"
            "       %s [-c config] --user NAME [--role admin|lesezeichen|station|nutzer]\n"
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
        { "no-ddc", no_argument, NULL, 'D' },
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
        case 'D': no_ddc = 1; break;
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
        cl[i].dview = cl[i].dseg = -1;
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
        for (int s = 0; s < MAX_SEG; s++)
            v->seg_rx[s] = -1;
        v->band = i < cfg.nbands ? i : -1;
        v->used = i < cfg.nbands;
        if (i < cfg.nbands) {
            v->center = cfg.bands[i].center;
            v->input = cfg.bands[i].input;
        }
        for (int s = 0; s < MAX_SEG; s++)
            if (wf_init(&v->wf[s], navg) < 0) {
                logmsg("out of memory");
                return 1;
            }
    }

    if (start_listener_threads() < 0) {
        logmsg("cannot start the listener thread");
        return 1;
    }

    int sfd = hw_open_stream(cfg.ifname);
    int lfd = net_listen(cfg.port);
    if (sfd < 0 || lfd < 0)
        return 1;
    logmsg("r2t2sdr %s listening on port %d, %d bands, www %s", VERSION, cfg.port, cfg.nbands, cfg.www);

    struct pollfd pfd[2 + MAX_CLIENTS];
    int map[2 + MAX_CLIENTS];
    time_t last_status = time(NULL), last_stats = last_status, last_tick = last_status;
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

        if (pfd[1].revents & POLLERR) {
            static time_t last_err;
            int err = hw_stream_clear_error(sfd);
            if (now_s() - last_err >= 60) {
                logmsg("stream socket error %d (%s), cleared", err, err > 0 ? strerror(err) : "error queue");
                last_err = now_s();
            }
        }

        npkt += hw_stream_read(on_packet);
        drain_audio();

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
                dsp_lock();
                memset(c, 0, sizeof(*c));
                c->fd = fd;
                c->view = -1;
                c->rx = -1;
                c->dview = c->dseg = -1;
                c->last_rx = now_s();
                snprintf(c->ip, sizeof(c->ip), "%s", ip);
                dsp_unlock();
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
                dsp_lock();
                int r = net_read(c, cfg.www, &callbacks);
                dsp_unlock();
                if (r < 0) {
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
        if (now != last_tick) {
            last_tick = now;
            dsp_lock();
            widen_tick();
            dsp_unlock();
        }
        if (now - last_status >= 5) {
            last_status = now;
            broadcast_status();
        }
        if (now - last_stats >= 60) {
            unsigned drops = hw_stream_drops(sfd);
            unsigned bd = 0;
            for (int w = 0; w < NWORK; w++) {
                bd += work[w].drops;
                work[w].drops = 0;
            }
            if (verbose || drops || bd)
                logmsg("stream: %lu frames/min, %u kernel drops, %u listener blocks dropped", npkt, drops, bd);
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
