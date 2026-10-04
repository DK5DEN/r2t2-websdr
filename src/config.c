#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
    return s;
}

static void copy(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%s", src);
    utf8_trim(dst);
}

/* "name, centre_hz [, input [, mode]]" */
/* IARU region 1 edges, used when a band line gives none */
static const double band_table[][2] = {
    { 1810000, 2000000 }, { 3500000, 3800000 }, { 5351500, 5366500 }, { 5900000, 6200000 },
    { 7000000, 7200000 }, { 7200000, 7450000 }, { 9400000, 9900000 }, { 10100000, 10150000 },
    { 14000000, 14350000 }, { 18068000, 18168000 }, { 21000000, 21450000 }, { 24890000, 24990000 },
    { 26965000, 27405000 }, { 28000000, 29700000 }, { 50000000, 52000000 },
};

static void band_edges(double center, double *lo, double *hi)
{
    *lo = *hi = 0;
    for (size_t i = 0; i < sizeof(band_table) / sizeof(band_table[0]); i++)
        if (center >= band_table[i][0] && center <= band_table[i][1]) {
            *lo = band_table[i][0];
            *hi = band_table[i][1];
            return;
        }
}

static int add_band(config_t *c, char *spec)
{
    if (c->nbands >= MAX_BANDS)
        return -1;

    band_cfg_t *b = &c->bands[c->nbands];
    memset(b, 0, sizeof(*b));
    b->input = 1;
    copy(b->mode, sizeof(b->mode), "usb");

    char *save = NULL;
    int i = 0;
    for (char *tok = strtok_r(spec, ",", &save); tok; tok = strtok_r(NULL, ",", &save), i++) {
        tok = trim(tok);
        switch (i) {
        case 0: copy(b->name, sizeof(b->name), tok); break;
        case 1: b->center = atof(tok); break;
        case 2: b->input = atoi(tok); break;
        case 3: copy(b->mode, sizeof(b->mode), tok); break;
        case 4: b->lo = atof(tok); break;
        case 5: b->hi = atof(tok); break;
        default: break;
        }
    }
    if (i < 2 || b->center <= 0 || b->center > 61.44e6)
        return -1;
    if (b->input < 1 || b->input > 3)
        b->input = 1;
    if (!(b->lo < b->center && b->center < b->hi))
        band_edges(b->center, &b->lo, &b->hi);
    c->nbands++;
    return 0;
}

static const char *default_bands[] = {
    "160m, 1900000, 1, lsb",
    "80m, 3650000, 1, lsb",
    "60m, 5359000, 1, usb",
    "49m Rundfunk, 6050000, 1, am",
    "40m, 7100000, 1, lsb",
    "41m Rundfunk, 7350000, 1, am",
    "31m Rundfunk, 9600000, 1, am",
    "30m, 10125000, 1, cw",
    "20m, 14175000, 1, usb",
    "17m, 18118000, 1, usb",
    "15m, 21225000, 1, usb",
    "12m, 24940000, 1, usb",
    "11m CB, 27205000, 1, am",
    "10m, 28500000, 1, usb",
    "6m, 50100000, 1, usb",
    NULL
};

void config_defaults(config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->port = 8073;
    copy(c->www, sizeof(c->www), "/opt/r2t2sdr/www");
    copy(c->ifname, sizeof(c->ifname), "rad0");
    copy(c->title, sizeof(c->title), "R2T2 WebSDR");
    c->clock = 122.88e6;
    c->wf_fps = 10;
    c->max_clients = 20;
    c->max_listeners = 14;
    copy(c->state_dir, sizeof(c->state_dir), "/var/lib/r2t2sdr");
    c->chat = 2;
}

/* station settings changed in the web interface, stored apart from the config file */
static const char *STATION_KEYS[] = { "title", "callsign", "location", "locator", "access", "chat",
                                      "gain1", "gain2", "att1", "att2", NULL };

static int station_key(const char *k)
{
    for (int i = 0; STATION_KEYS[i]; i++)
        if (!strcmp(k, STATION_KEYS[i]))
            return 1;
    return 0;
}

void config_default_bands(config_t *c)
{
    char tmp[128];
    for (int i = 0; default_bands[i]; i++) {
        copy(tmp, sizeof(tmp), default_bands[i]);
        add_band(c, tmp);
    }
}

static int load_file(config_t *c, const char *path, int station_only)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    char line[512];
    int ln = 0;
    while (fgets(line, sizeof(line), f)) {
        ln++;
        char *hash = strchr(line, '#');
        if (hash)
            *hash = 0;
        char *s = trim(line);
        if (!*s)
            continue;
        char *eq = strchr(s, '=');
        if (!eq) {
            fprintf(stderr, "%s:%d: missing '='\n", path, ln);
            continue;
        }
        *eq = 0;
        char *k = trim(s), *v = trim(eq + 1);
        if (station_only && !station_key(k))
            continue;

        if (!strcmp(k, "port"))                c->port = atoi(v);
        else if (!strcmp(k, "state_dir"))      copy(c->state_dir, sizeof(c->state_dir), v);
        else if (!strcmp(k, "access"))         c->access = !strcmp(v, "login");
        else if (!strcmp(k, "chat"))           c->chat = !strcmp(v, "off") ? 0 : !strcmp(v, "login") ? 1 : 2;
        else if (!strcmp(k, "www"))            copy(c->www, sizeof(c->www), v);
        else if (!strcmp(k, "interface"))      copy(c->ifname, sizeof(c->ifname), v);
        else if (!strcmp(k, "title"))          copy(c->title, sizeof(c->title), v);
        else if (!strcmp(k, "callsign"))       copy(c->callsign, sizeof(c->callsign), v);
        else if (!strcmp(k, "location"))       copy(c->location, sizeof(c->location), v);
        else if (!strcmp(k, "locator"))        copy(c->locator, sizeof(c->locator), v);
        else if (!strcmp(k, "clock"))          c->clock = atof(v);
        else if (!strcmp(k, "gain1"))          c->gain[0] = atoi(v);
        else if (!strcmp(k, "gain2"))          c->gain[1] = atoi(v);
        else if (!strcmp(k, "att1"))           c->att[0] = atoi(v);
        else if (!strcmp(k, "att2"))           c->att[1] = atoi(v);
        else if (!strcmp(k, "waterfall_fps"))  c->wf_fps = atoi(v);
        else if (!strcmp(k, "max_clients"))    c->max_clients = atoi(v);
        else if (!strcmp(k, "max_listeners"))  c->max_listeners = atoi(v);
        else if (!strcmp(k, "band")) {
            if (add_band(c, v) < 0)
                fprintf(stderr, "%s:%d: invalid band definition\n", path, ln);
        } else
            fprintf(stderr, "%s:%d: unknown key '%s'\n", path, ln, k);
    }
    fclose(f);

    if (c->max_clients < 1 || c->max_clients > MAX_CLIENTS)
        c->max_clients = MAX_CLIENTS;
    if (c->max_listeners < 1 || c->max_listeners > MAX_CLIENTS)
        c->max_listeners = MAX_CLIENTS;
    if (c->wf_fps < 1)
        c->wf_fps = 1;
    return 0;
}

int config_load(config_t *c, const char *path)
{
    return load_file(c, path, 0);
}

int config_load_station(config_t *c)
{
    char path[300];
    snprintf(path, sizeof(path), "%s/station.conf", c->state_dir);
    return load_file(c, path, 1);
}

int config_save_station(const config_t *c)
{
    char path[300], tmp[310];
    snprintf(path, sizeof(path), "%s/station.conf", c->state_dir);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return -1;
    fprintf(f, "# written by r2t2sdr (Verwaltung > Station), overrides /etc/r2t2sdr.conf\n");
    fprintf(f, "title = %s\ncallsign = %s\nlocation = %s\nlocator = %s\naccess = %s\nchat = %s\n",
            c->title, c->callsign, c->location, c->locator, c->access ? "login" : "open",
            c->chat == 0 ? "off" : c->chat == 1 ? "login" : "all");
    fprintf(f, "gain1 = %d\ngain2 = %d\natt1 = %d\natt2 = %d\n", c->gain[0], c->gain[1], c->att[0], c->att[1]);
    int ok = fclose(f) == 0;
    if (!ok || rename(tmp, path) < 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}
