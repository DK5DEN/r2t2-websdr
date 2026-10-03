#ifndef NET_H
#define NET_H

#include <stddef.h>
#include <stdint.h>

#include "dsp.h"

#define NET_IN_MAX     8192
#define NET_OUT_MAX    (1 << 20)   /* close clients that fall this far behind */
#define NET_DROP_LIMIT (128 << 10) /* skip droppable frames above this backlog */

typedef struct client {
    int fd;
    int ws;
    int closing;
    char ip[48];
    char in[NET_IN_MAX];
    size_t inlen;
    uint8_t *out;
    size_t outoff, outlen, outcap;
    unsigned dropped;
    long last_rx;      /* monotonic seconds of the last received data */

    /* application state */
    char user[32];     /* logged-in account, "" = anonymous */
    int role;          /* ROLE_* from auth.h */
    long chat_t;       /* chat rate limit: start of the current minute ... */
    int chat_n;        /* ... and messages in it */
    char nonce[33];    /* open login challenge, "" = none */
    char nonce_user[32];
    long nonce_t;
    int view;          /* waterfall view, -1 = none */
    int rx;            /* receiver for audio, -1 = none */
    int input;         /* antenna input of that receiver, 0 = not set */
    int listening;
    double freq;
    int mode;
    float lo, hi;
    float sql;
    demod_t dm;
} client_t;

typedef struct {
    void (*on_open)(client_t *c);
    void (*on_text)(client_t *c, char *txt, size_t len);
    int (*on_http)(client_t *c, const char *path);   /* 1 = answered */
} net_cb_t;

void net_http_reply(client_t *c, int code, const char *ctype, const void *body, size_t n);

int net_listen(int port);
int net_accept(int lfd, char *ip, size_t iplen);
void net_queue(client_t *c, const void *data, size_t n);
int net_ws_send(client_t *c, int opcode, const void *data, size_t n, int droppable);
int net_flush(client_t *c);
int net_read(client_t *c, const char *www, const net_cb_t *cb);

#endif
