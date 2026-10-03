/*
 * Station bookmarks, shown to every visitor.
 * <dir>/bookmarks: one line per bookmark, tab separated: id freq mode name
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bookmarks.h"
#include "common.h"

static char path[300];
static bookmark_t bms[MAX_BOOKMARKS];
static int nbms, next_id = 1;

static void clean(char *dst, size_t n, const char *src)
{
    size_t o = 0;
    for (; *src && o + 1 < n; src++)
        dst[o++] = (*src == '\t' || *src == '\n' || *src == '\r') ? ' ' : *src;
    dst[o] = 0;
    utf8_trim(dst);
}

static int cmp(const void *a, const void *b)
{
    double d = ((const bookmark_t *)a)->freq - ((const bookmark_t *)b)->freq;
    return d < 0 ? -1 : d > 0;
}

static int save(void);

int bm_save(void)
{
    qsort(bms, nbms, sizeof(bms[0]), cmp);
    return save();
}

void bm_clear(void)
{
    nbms = 0;
}

int bm_add(double freq, const char *mode, const char *name)
{
    char nm[sizeof(bms[0].name)];
    clean(nm, sizeof(nm), name);
    for (int i = 0; i < nbms; i++)
        if (bms[i].freq > freq - 1 && bms[i].freq < freq + 1 && !strcmp(bms[i].name, nm))
            return 0;
    if (nbms >= MAX_BOOKMARKS)
        return -1;
    bookmark_t *b = &bms[nbms++];
    memset(b, 0, sizeof(*b));
    b->id = next_id++;
    b->freq = freq;
    snprintf(b->mode, sizeof(b->mode), "%s", mode);
    snprintf(b->name, sizeof(b->name), "%s", nm);
    return 1;
}

static int save(void)
{
    char tmp[320];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    FILE *f = fd < 0 ? NULL : fdopen(fd, "w");
    if (!f)
        return -1;
    for (int i = 0; i < nbms; i++)
        fprintf(f, "%d\t%.0f\t%s\t%s\n", bms[i].id, bms[i].freq, bms[i].mode, bms[i].name);
    int ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    fclose(f);
    if (!ok || rename(tmp, path) < 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

int bm_init(const char *dir)
{
    snprintf(path, sizeof(path), "%s/bookmarks", dir);
    nbms = 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    char line[256];
    while (fgets(line, sizeof(line), f) && nbms < MAX_BOOKMARKS) {
        bookmark_t b;
        memset(&b, 0, sizeof(b));
        char *save_p = NULL;
        char *id = strtok_r(line, "\t\n", &save_p);
        char *fr = strtok_r(NULL, "\t\n", &save_p);
        char *mo = strtok_r(NULL, "\t\n", &save_p);
        char *nm = strtok_r(NULL, "\n", &save_p);
        if (!id || !fr || !mo || !nm)
            continue;
        b.id = atoi(id);
        b.freq = atof(fr);
        snprintf(b.mode, sizeof(b.mode), "%s", mo);
        clean(b.name, sizeof(b.name), nm);
        if (b.id <= 0 || b.freq <= 0)
            continue;
        bms[nbms++] = b;
        if (b.id >= next_id)
            next_id = b.id + 1;
    }
    fclose(f);
    qsort(bms, nbms, sizeof(bms[0]), cmp);
    return 0;
}

/* id 0 creates a new bookmark; returns the id or -1 */
int bm_set(int id, double freq, const char *mode, const char *name)
{
    bookmark_t *b = NULL;
    if (id > 0) {
        for (int i = 0; i < nbms; i++)
            if (bms[i].id == id)
                b = &bms[i];
        if (!b)
            return -1;
    } else {
        if (nbms >= MAX_BOOKMARKS)
            return -1;
        b = &bms[nbms++];
        memset(b, 0, sizeof(*b));
        b->id = next_id++;
    }
    b->freq = freq;
    snprintf(b->mode, sizeof(b->mode), "%s", mode);
    clean(b->name, sizeof(b->name), name);
    id = b->id;
    qsort(bms, nbms, sizeof(bms[0]), cmp);
    return save() < 0 ? -1 : id;
}

int bm_del(int id)
{
    for (int i = 0; i < nbms; i++)
        if (bms[i].id == id) {
            memmove(&bms[i], &bms[i + 1], (nbms - i - 1) * sizeof(bms[0]));
            nbms--;
            return save();
        }
    return -1;
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

int bm_json(char *buf, size_t n)
{
    int o = snprintf(buf, n, "{\"type\":\"bookmarks\",\"list\":[");
    for (int i = 0; i < nbms && o < (int)n - 200; i++) {
        char nm[140];
        esc(nm, sizeof(nm), bms[i].name);
        o += snprintf(buf + o, n - o, "%s{\"id\":%d,\"freq\":%.0f,\"mode\":\"%s\",\"name\":\"%s\"}",
                      i ? "," : "", bms[i].id, bms[i].freq, bms[i].mode, nm);
    }
    o += snprintf(buf + o, n - o, "]}");
    return o;
}
