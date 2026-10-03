#ifndef BOOKMARKS_H
#define BOOKMARKS_H

#include <stddef.h>

#define MAX_BOOKMARKS 200

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

#endif
