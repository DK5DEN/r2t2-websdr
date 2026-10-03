#ifndef ANTENNAS_H
#define ANTENNAS_H

#include <stddef.h>

#define NANT        2      /* antenna inputs ANT1, ANT2 (one ADC each) */
#define MAX_RANGES  16

typedef struct {
    char name[48];
    int nranges;
    double lo[MAX_RANGES], hi[MAX_RANGES];   /* Hz */
} antenna_t;

int ant_init(const char *dir);
int ant_for(double freq);                 /* input 1..NANT */
int ant_default(void);
int ant_configured(void);                 /* any ranges set */
const antenna_t *ant_get(int input);
int ant_set(int input, const char *name, const char *ranges);
int ant_set_default(int input);
int ant_ranges_text(int input, char *buf, size_t n);
int ant_json(char *buf, size_t n, const int gain[2], const int att[2]);

#endif
