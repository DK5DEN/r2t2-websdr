/*
 * Which antenna input receives which frequencies.
 *
 * <dir>/antennas:
 *   default <input>
 *   <input>\t<name>\t<lo_khz>-<hi_khz>,<lo_khz>-<hi_khz>,...
 *
 * A frequency goes to the first input with a range covering it, otherwise to
 * the default input.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "antennas.h"
#include "common.h"

static char path[300];
static antenna_t ants[NANT];
static int def_input = 1;

static int parse_ranges(antenna_t *a, const char *text)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", text);
    a->nranges = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",; \t\n", &save); tok; tok = strtok_r(NULL, ",; \t\n", &save)) {
        double lo, hi;
        if (sscanf(tok, "%lf-%lf", &lo, &hi) != 2 || lo < 0 || hi <= lo || hi > 62000)
            return -1;
        if (a->nranges >= MAX_RANGES)
            return -1;
        a->lo[a->nranges] = lo * 1000;
        a->hi[a->nranges] = hi * 1000;
        a->nranges++;
    }
    return 0;
}

int ant_ranges_text(int input, char *buf, size_t n)
{
    const antenna_t *a = ant_get(input);
    int o = 0;
    buf[0] = 0;
    for (int i = 0; a && i < a->nranges && o < (int)n - 32; i++)
        o += snprintf(buf + o, n - o, "%s%.0f-%.0f", i ? "," : "", a->lo[i] / 1000, a->hi[i] / 1000);
    return o;
}

static int save(void)
{
    char tmp[320];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    FILE *f = fd < 0 ? NULL : fdopen(fd, "w");
    if (!f)
        return -1;
    fprintf(f, "default %d\n", def_input);
    for (int i = 0; i < NANT; i++) {
        char r[512];
        ant_ranges_text(i + 1, r, sizeof(r));
        fprintf(f, "%d\t%s\t%s\n", i + 1, ants[i].name, r);
    }
    int ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    fclose(f);
    if (!ok || rename(tmp, path) < 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

int ant_init(const char *dir)
{
    snprintf(path, sizeof(path), "%s/antennas", dir);
    memset(ants, 0, sizeof(ants));
    for (int i = 0; i < NANT; i++)
        snprintf(ants[i].name, sizeof(ants[i].name), "Antenne %d", i + 1);
    def_input = 1;

    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    char line[700];
    while (fgets(line, sizeof(line), f)) {
        int in;
        if (sscanf(line, "default %d", &in) == 1) {
            if (in >= 1 && in <= NANT)
                def_input = in;
            continue;
        }
        char *save_p = NULL;
        char *a = strtok_r(line, "\t\n", &save_p);
        char *nm = strtok_r(NULL, "\t\n", &save_p);
        char *rg = strtok_r(NULL, "\t\n", &save_p);
        if (!a || !nm)
            continue;
        in = atoi(a);
        if (in < 1 || in > NANT)
            continue;
        snprintf(ants[in - 1].name, sizeof(ants[in - 1].name), "%s", nm);
        if (rg)
            parse_ranges(&ants[in - 1], rg);
    }
    fclose(f);
    return 0;
}

int ant_for(double freq)
{
    for (int i = 0; i < NANT; i++)
        for (int r = 0; r < ants[i].nranges; r++)
            if (freq >= ants[i].lo[r] && freq <= ants[i].hi[r])
                return i + 1;
    return def_input;
}

int ant_default(void) { return def_input; }

int ant_configured(void)
{
    for (int i = 0; i < NANT; i++)
        if (ants[i].nranges)
            return 1;
    return 0;
}

const antenna_t *ant_get(int input)
{
    return input >= 1 && input <= NANT ? &ants[input - 1] : NULL;
}

/* ranges as text in kHz, e.g. "1810-2000,3500-3800" */
int ant_set(int input, const char *name, const char *ranges)
{
    if (input < 1 || input > NANT)
        return -1;
    antenna_t a;
    memset(&a, 0, sizeof(a));
    size_t o = 0;
    for (const char *s = name; *s && o + 1 < sizeof(a.name); s++)
        a.name[o++] = (*s == '\t' || *s == '\n' || *s == '\r') ? ' ' : *s;
    utf8_trim(a.name);
    if (!o)
        snprintf(a.name, sizeof(a.name), "Antenne %d", input);
    if (parse_ranges(&a, ranges) < 0)
        return -1;
    ants[input - 1] = a;
    return save();
}

int ant_set_default(int input)
{
    if (input < 1 || input > NANT)
        return -1;
    def_input = input;
    return save();
}

static void esc(char *dst, size_t n, const char *s)
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

int ant_json(char *buf, size_t n, const int gain[2], const int att[2])
{
    int o = snprintf(buf, n, "{\"type\":\"antennas\",\"default\":%d,\"list\":[", def_input);
    for (int i = 0; i < NANT; i++) {
        char nm[110], r[512];
        esc(nm, sizeof(nm), ants[i].name);
        ant_ranges_text(i + 1, r, sizeof(r));
        o += snprintf(buf + o, n - o,
                      "%s{\"input\":%d,\"name\":\"%s\",\"ranges\":\"%s\",\"gain\":%d,\"att\":%d}",
                      i ? "," : "", i + 1, nm, r, gain[i], att[i]);
    }
    o += snprintf(buf + o, n - o, "]}");
    return o;
}
