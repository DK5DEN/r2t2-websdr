#ifndef CONFIG_H
#define CONFIG_H

#include "common.h"

typedef struct {
    char name[32];
    double center;      /* waterfall centre frequency in Hz */
    int input;          /* antenna input 1..3 */
    char mode[8];       /* default mode: lsb, usb, cw, am, fm */
} band_cfg_t;

typedef struct {
    int port;
    char www[256];
    char ifname[16];
    char title[96];
    char callsign[24];
    char location[96];
    char locator[12];
    double clock;
    int gain[2];        /* PGA gain per ADC in dB, -9..32 */
    int att[2];         /* attenuator per ADC in dB, 0..31 */
    int wf_fps;
    int max_clients;
    band_cfg_t bands[MAX_BANDS];
    int nbands;
} config_t;

void config_defaults(config_t *c);
void config_default_bands(config_t *c);
int config_load(config_t *c, const char *path);

#endif
