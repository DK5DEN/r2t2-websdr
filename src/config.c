#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
}

/* "name, centre_hz [, input [, mode]]" */
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
        default: break;
        }
    }
    if (i < 2 || b->center <= 0 || b->center > 61.44e6)
        return -1;
    if (b->input < 1 || b->input > 3)
        b->input = 1;
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
}

void config_default_bands(config_t *c)
{
    char tmp[128];
    for (int i = 0; default_bands[i]; i++) {
        copy(tmp, sizeof(tmp), default_bands[i]);
        add_band(c, tmp);
    }
}

int config_load(config_t *c, const char *path)
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

        if (!strcmp(k, "port"))                c->port = atoi(v);
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
        else if (!strcmp(k, "band")) {
            if (add_band(c, v) < 0)
                fprintf(stderr, "%s:%d: invalid band definition\n", path, ln);
        } else
            fprintf(stderr, "%s:%d: unknown key '%s'\n", path, ln, k);
    }
    fclose(f);

    if (c->max_clients < 1 || c->max_clients > MAX_CLIENTS)
        c->max_clients = MAX_CLIENTS;
    if (c->wf_fps < 1)
        c->wf_fps = 1;
    return 0;
}
