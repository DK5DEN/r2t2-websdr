#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net.h"
#include "sha1.h"

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define FILE_MAX (4 << 20)

static void set_nonblock(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

int net_listen(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 16) < 0) {
        perror("bind/listen");
        close(fd);
        return -1;
    }
    set_nonblock(fd);
    return fd;
}

int net_accept(int lfd, char *ip, size_t iplen)
{
    struct sockaddr_in a;
    socklen_t l = sizeof(a);
    int fd = accept(lfd, (struct sockaddr *)&a, &l);
    if (fd < 0)
        return -1;
    set_nonblock(fd);
    int one = 1, idle = 30, intvl = 10, cnt = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
    inet_ntop(AF_INET, &a.sin_addr, ip, iplen);
    return fd;
}

/* ---------------------------------------------------------------- output */

static int out_reserve(client_t *c, size_t n)
{
    if (c->outoff && c->outoff + c->outlen + n > c->outcap) {
        memmove(c->out, c->out + c->outoff, c->outlen);
        c->outoff = 0;
    }
    if (c->outlen + n > c->outcap) {
        size_t nc = c->outcap ? c->outcap : 16384;
        while (nc < c->outlen + n)
            nc *= 2;
        if (nc > NET_OUT_MAX)
            return -1;
        uint8_t *p = realloc(c->out, nc);
        if (!p)
            return -1;
        c->out = p;
        c->outcap = nc;
    }
    return 0;
}

void net_queue(client_t *c, const void *data, size_t n)
{
    if (c->closing && c->ws)
        return;
    if (out_reserve(c, n) < 0) {
        c->closing = 1;
        return;
    }
    memcpy(c->out + c->outoff + c->outlen, data, n);
    c->outlen += n;
}

int net_ws_send(client_t *c, int opcode, const void *data, size_t n, int droppable)
{
    if (!c->ws || c->closing)
        return 0;
    if (droppable && c->outlen > NET_DROP_LIMIT) {
        c->dropped++;
        return 0;
    }
    uint8_t h[10];
    size_t hl;
    h[0] = 0x80 | opcode;
    if (n < 126) {
        h[1] = (uint8_t)n;
        hl = 2;
    } else if (n < 65536) {
        h[1] = 126;
        h[2] = (uint8_t)(n >> 8);
        h[3] = (uint8_t)n;
        hl = 4;
    } else {
        h[1] = 127;
        for (int i = 0; i < 8; i++)
            h[2 + i] = (uint8_t)((uint64_t)n >> (56 - 8 * i));
        hl = 10;
    }
    net_queue(c, h, hl);
    net_queue(c, data, n);
    return 1;
}

int net_flush(client_t *c)
{
    while (c->outlen) {
        ssize_t r = send(c->fd, c->out + c->outoff, c->outlen, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 0;
            if (errno == EINTR)
                continue;
            return -1;
        }
        c->outoff += r;
        c->outlen -= r;
    }
    c->outoff = 0;
    return 0;
}

/* ---------------------------------------------------------------- http */

static const char *mime(const char *path)
{
    const char *e = strrchr(path, '.');
    if (!e) return "application/octet-stream";
    if (!strcmp(e, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(e, ".js"))   return "application/javascript; charset=utf-8";
    if (!strcmp(e, ".css"))  return "text/css; charset=utf-8";
    if (!strcmp(e, ".svg"))  return "image/svg+xml";
    if (!strcmp(e, ".png"))  return "image/png";
    if (!strcmp(e, ".ico"))  return "image/x-icon";
    if (!strcmp(e, ".json")) return "application/json";
    return "application/octet-stream";
}

static const char *reason(int code)
{
    switch (code) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    default:  return "Internal Server Error";
    }
}

void net_http_reply(client_t *c, int code, const char *ctype, const void *body, size_t n)
{
    char h[320];
    int hl = snprintf(h, sizeof(h),
                      "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                      "Cache-Control: no-cache\r\nAccess-Control-Allow-Origin: *\r\n"
                      "Connection: close\r\n\r\n",
                      code, reason(code), ctype, n);
    net_queue(c, h, hl);
    net_queue(c, body, n);
    c->closing = 1;
}

static void http_error(client_t *c, int code, const char *msg)
{
    net_http_reply(c, code, "text/plain; charset=utf-8", msg, strlen(msg));
}

static void http_file(client_t *c, const char *www, const char *path)
{
    if (path[0] != '/' || strstr(path, "..")) {
        http_error(c, 400, "Bad Request");
        return;
    }
    if (!strcmp(path, "/"))
        path = "/index.html";

    char fn[512];
    snprintf(fn, sizeof(fn), "%s%s", www, path);
    FILE *f = fopen(fn, "rb");
    if (!f) {
        http_error(c, 404, "Not Found");
        return;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *buf = n > 0 && n < FILE_MAX ? malloc(n) : NULL;
    if (!buf || fread(buf, 1, n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        http_error(c, 500, "Internal Server Error");
        return;
    }
    fclose(f);

    /* pages are always fresh; style sheets, scripts and images carry ?v= stamps in the page */
    const char *e = strrchr(path, '.');
    const char *cache = e && !strcmp(e, ".html") ? "no-cache" : "public, max-age=604800";
    char h[300];
    int hl = snprintf(h, sizeof(h),
                      "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\n"
                      "Cache-Control: %s\r\nConnection: close\r\n\r\n",
                      mime(path), n, cache);
    net_queue(c, h, hl);
    net_queue(c, buf, n);
    free(buf);
    c->closing = 1;
}

/* case-insensitive header lookup in a NUL-terminated request head */
static int header(const char *req, const char *name, char *out, size_t n)
{
    size_t nl = strlen(name);
    for (const char *p = strstr(req, "\r\n"); p; p = strstr(p + 2, "\r\n")) {
        const char *l = p + 2;
        if (!strncasecmp(l, name, nl) && l[nl] == ':') {
            l += nl + 1;
            while (*l == ' ' || *l == '\t')
                l++;
            size_t i = 0;
            while (l[i] && l[i] != '\r' && i + 1 < n) {
                out[i] = l[i];
                i++;
            }
            out[i] = 0;
            return 1;
        }
    }
    return 0;
}

static void ws_accept(client_t *c, const char *key)
{
    char k[160];
    snprintf(k, sizeof(k), "%s" WS_GUID, key);
    uint8_t d[20];
    sha1(k, strlen(k), d);
    char acc[32];
    base64(d, 20, acc);
    char r[256];
    int n = snprintf(r, sizeof(r),
                     "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                     "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n",
                     acc);
    net_queue(c, r, n);
    c->ws = 1;
}

/* ---------------------------------------------------------------- input */

static int handle_http(client_t *c, const char *www, const net_cb_t *cb)
{
    c->in[c->inlen] = 0;
    char *end = strstr(c->in, "\r\n\r\n");
    if (!end)
        return c->inlen >= NET_IN_MAX - 1 ? -1 : 0;
    *end = 0;
    size_t hlen = end + 4 - c->in;

    char method[8], path[256];
    if (sscanf(c->in, "%7s %255s", method, path) != 2) {
        http_error(c, 400, "Bad Request");
        return 0;
    }
    char *q = strchr(path, '?');
    if (q)
        *q = 0;
    if (strcmp(method, "GET")) {
        http_error(c, 405, "Method Not Allowed");
        return 0;
    }

    char key[64];
    if (!strcmp(path, "/ws") && header(c->in, "Sec-WebSocket-Key", key, sizeof(key))) {
        ws_accept(c, key);
        memmove(c->in, c->in + hlen, c->inlen - hlen);
        c->inlen -= hlen;
        if (cb->on_open)
            cb->on_open(c);
        return 0;
    }
    if (!(cb->on_http && cb->on_http(c, path)))
        http_file(c, www, path);
    c->inlen = 0;
    return 0;
}

static int handle_ws(client_t *c, const net_cb_t *cb)
{
    while (c->inlen >= 2) {
        uint8_t *b = (uint8_t *)c->in;
        int op = b[0] & 0x0f;
        size_t len = b[1] & 0x7f;
        size_t hl = 2;
        if (!(b[1] & 0x80))
            return -1;                      /* client frames must be masked */
        if (len == 126) {
            if (c->inlen < 4)
                break;
            len = (size_t)b[2] << 8 | b[3];
            hl = 4;
        } else if (len == 127) {
            return -1;
        }
        if (len > 4096)
            return -1;
        if (c->inlen < hl + 4 + len)
            break;

        uint8_t *mask = b + hl, *p = b + hl + 4;
        for (size_t i = 0; i < len; i++)
            p[i] ^= mask[i & 3];

        switch (op) {
        case 1: {
            char txt[4097];
            memcpy(txt, p, len);
            txt[len] = 0;
            if (cb->on_text)
                cb->on_text(c, txt, len);
            break;
        }
        case 8: {
            uint8_t h[2] = { 0x88, 0 };
            net_queue(c, h, 2);
            c->closing = 1;
            break;
        }
        case 9:
            net_ws_send(c, 10, p, len, 0);
            break;
        default:
            break;
        }
        size_t fl = hl + 4 + len;
        memmove(c->in, c->in + fl, c->inlen - fl);
        c->inlen -= fl;
    }
    return 0;
}

int net_read(client_t *c, const char *www, const net_cb_t *cb)
{
    ssize_t r = recv(c->fd, c->in + c->inlen, NET_IN_MAX - 1 - c->inlen, 0);
    if (r == 0)
        return -1;
    if (r < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
    c->inlen += r;

    if (!c->ws) {
        if (handle_http(c, www, cb) < 0)
            return -1;
        if (!c->ws)
            return 0;
    }
    return handle_ws(c, cb);
}
