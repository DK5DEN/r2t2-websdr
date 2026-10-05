#ifndef CONFIG_H
#define CONFIG_H

#include "common.h"

typedef struct {
    char name[32];
    double center;      /* waterfall centre frequency in Hz */
    int input;          /* antenna input 1..3 */
    char mode[8];       /* default mode: lsb, usb, cw, am, fm */
    double lo, hi;      /* band edges in Hz for widening, 0 = one receiver only */
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
    int max_listeners;  /* CPU guard for listeners fed from the wide stream */
    char state_dir[256];  /* accounts, sessions, bookmarks, station settings */
    int access;           /* 0 = everyone may listen, 1 = only logged-in users */
    int chat;             /* 0 = off, 1 = logged-in users, 2 = everyone */
    int log;              /* activity log in state_dir/activity.log, 0 = off */
    /* engine mode for afu-remote: sessions with a ticket signed by the station */
    char ticket_secret[256];  /* file with the shared secret, "" = no tickets */
    char ticket_device[24];   /* receiver id the tickets must name */
    int ticket_only;          /* 1: WebSocket only with a valid ticket */
    char control_socket[256]; /* Unix socket for afu-remote (status, online), "" = off */
    char control_group[32];   /* group that may use it, "" = root only */
    band_cfg_t bands[MAX_BANDS];
    int nbands;
} config_t;

void config_defaults(config_t *c);
void config_default_bands(config_t *c);
int config_load(config_t *c, const char *path);
int config_load_station(config_t *c);
int config_save_station(const config_t *c);

#endif
