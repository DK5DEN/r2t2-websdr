#ifndef BOOKMARKS_H
#define BOOKMARKS_H

#include <stddef.h>

#define MAX_BOOKMARKS 1000

typedef struct {
    int id;
    double freq;
    char mode[8];
    char name[64];
} bookmark_t;

int bm_init(const char *dir);
int bm_set(int id, double freq, const char *mode, const char *name);
int bm_del(int id);
int bm_json(char *buf, size_t n);

/* bulk import: add without saving (1 added, 0 duplicate, -1 full), then bm_save() */
void bm_clear(void);
int bm_add(double freq, const char *mode, const char *name);
int bm_save(void);

#endif
