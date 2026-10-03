/*
 * Accounts and login sessions.
 *
 * <dir>/users     one line per account: name role iterations salt-hex hash-hex
 *                 (PBKDF2-HMAC-SHA256, 16 byte salt)
 * <dir>/sessions  one line per session: sha256(token)-hex name expiry
 *
 * Only hashes of passwords and tokens are stored. Both files are written
 * with mode 0600 through a temporary file and rename.
 */
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "auth.h"
#include "sha256.h"

#define PBKDF2_ITER     AUTH_ITER
#define MAX_SESSIONS    256
#define SESSION_DAYS    90

typedef struct {
    char hash[65];
    char name[NAME_MAX_LEN + 1];
    long expiry;
} session_t;

static char users_path[300], sessions_path[300];
static user_t users[MAX_USERS];
static int nusers;
static time_t users_mtime;
static session_t sessions[MAX_SESSIONS];
static int nsessions;

static void hex(const uint8_t *in, size_t n, char *out)
{
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = d[in[i] >> 4];
        out[2 * i + 1] = d[in[i] & 15];
    }
    out[2 * n] = 0;
}

static int unhex(const char *s, uint8_t *out, size_t max)
{
    size_t n = strlen(s) / 2;
    if (n > max)
        return -1;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1)
            return -1;
        out[i] = (uint8_t)v;
    }
    return (int)n;
}

static int random_bytes(uint8_t *buf, size_t n)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return -1;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r <= 0) {
            close(fd);
            return -1;
        }
        got += r;
    }
    close(fd);
    return 0;
}

/* constant-time string compare */
static int same(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned diff = (unsigned)(la ^ lb);
    for (size_t i = 0; i < la && i < lb; i++)
        diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return diff == 0;
}

/* ---------------------------------------------------------------- files */

static FILE *open_tmp(const char *path, char *tmp, size_t n)
{
    snprintf(tmp, n, "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    return fd < 0 ? NULL : fdopen(fd, "w");
}

static int commit_tmp(FILE *f, const char *tmp, const char *path)
{
    int ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    fclose(f);
    if (!ok || rename(tmp, path) < 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static time_t file_mtime(const char *p)
{
    struct stat sb;
    return stat(p, &sb) == 0 ? sb.st_mtime : 0;
}

static void load_users(void)
{
    nusers = 0;
    users_mtime = file_mtime(users_path);
    FILE *f = fopen(users_path, "r");
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof(line), f) && nusers < MAX_USERS) {
        user_t u;
        char role[16];
        memset(&u, 0, sizeof(u));
        if (sscanf(line, "%24s %15s %d %32s %64s", u.name, role, &u.iter, u.salt, u.hash) != 5)
            continue;
        u.role = role_parse(role);
        if (u.role == ROLE_NONE || !auth_valid_name(u.name))
            continue;
        users[nusers++] = u;
    }
    fclose(f);
}

/* accounts can also be changed with "r2t2sdr --user" while the service runs */
static void refresh(void)
{
    if (file_mtime(users_path) != users_mtime)
        load_users();
}

static int save_users(void)
{
    char tmp[320];
    FILE *f = open_tmp(users_path, tmp, sizeof(tmp));
    if (!f)
        return -1;
    for (int i = 0; i < nusers; i++)
        fprintf(f, "%s %s %d %s %s\n", users[i].name, role_name(users[i].role), users[i].iter,
                users[i].salt, users[i].hash);
    int r = commit_tmp(f, tmp, users_path);
    users_mtime = file_mtime(users_path);
    return r;
}

static int save_sessions(void)
{
    char tmp[320];
    FILE *f = open_tmp(sessions_path, tmp, sizeof(tmp));
    if (!f)
        return -1;
    for (int i = 0; i < nsessions; i++)
        fprintf(f, "%s %s %ld\n", sessions[i].hash, sessions[i].name, sessions[i].expiry);
    return commit_tmp(f, tmp, sessions_path);
}

int auth_init(const char *dir)
{
    mkdir(dir, 0700);
    snprintf(users_path, sizeof(users_path), "%s/users", dir);
    snprintf(sessions_path, sizeof(sessions_path), "%s/sessions", dir);
    nsessions = 0;
    load_users();

    char line[512];
    long now = time(NULL);
    FILE *f = fopen(sessions_path, "r");
    if (f) {
        while (fgets(line, sizeof(line), f) && nsessions < MAX_SESSIONS) {
            session_t s;
            memset(&s, 0, sizeof(s));
            if (sscanf(line, "%64s %24s %ld", s.hash, s.name, &s.expiry) != 3)
                continue;
            if (s.expiry > now && auth_user_find(s.name))
                sessions[nsessions++] = s;
        }
        fclose(f);
    }
    return 0;
}

/* ---------------------------------------------------------------- users */

const char *role_name(int role)
{
    switch (role) {
    case ROLE_ADMIN:  return "admin";
    case ROLE_EDITOR: return "lesezeichen";
    case ROLE_USER:   return "nutzer";
    default:          return "";
    }
}

int role_parse(const char *s)
{
    if (!strcmp(s, "admin"))       return ROLE_ADMIN;
    if (!strcmp(s, "lesezeichen")) return ROLE_EDITOR;
    if (!strcmp(s, "nutzer"))      return ROLE_USER;
    return ROLE_NONE;
}

/* letters, digits, '.', '-', '_'; callsigns fit */
int auth_valid_name(const char *name)
{
    size_t n = strlen(name);
    if (n < 2 || n > NAME_MAX_LEN)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)name[i]) && name[i] != '.' && name[i] != '-' && name[i] != '_')
            return 0;
    return 1;
}

int auth_user_count(void)
{
    refresh();
    return nusers;
}

int auth_admin_count(void)
{
    int n = 0;
    for (int i = 0; i < nusers; i++)
        n += users[i].role == ROLE_ADMIN;
    return n;
}

const user_t *auth_user_at(int i)
{
    return i >= 0 && i < nusers ? &users[i] : NULL;
}

static int user_index(const char *name)
{
    for (int i = 0; i < nusers; i++)
        if (!strcasecmp(users[i].name, name))
            return i;
    return -1;
}

const user_t *auth_user_find(const char *name)
{
    int i = user_index(name);
    return i < 0 ? NULL : &users[i];
}

static int set_password(user_t *u, const char *pass)
{
    uint8_t salt[16], dk[32];
    if (random_bytes(salt, sizeof(salt)) < 0)
        return -1;
    pbkdf2_sha256(pass, salt, sizeof(salt), PBKDF2_ITER, dk);
    u->iter = PBKDF2_ITER;
    hex(salt, sizeof(salt), u->salt);
    hex(dk, sizeof(dk), u->hash);
    return 0;
}

/* Create or change an account. pass NULL or "" keeps the password (new accounts need one). */
int auth_user_set(const char *name, int role, const char *pass)
{
    if (!auth_valid_name(name) || role == ROLE_NONE)
        return -1;
    refresh();
    int i = user_index(name);
    user_t u;
    if (i >= 0) {
        u = users[i];
    } else {
        if (!pass || !*pass || nusers >= MAX_USERS)
            return -1;
        memset(&u, 0, sizeof(u));
        snprintf(u.name, sizeof(u.name), "%s", name);
    }
    u.role = role;
    if (pass && *pass && set_password(&u, pass) < 0)
        return -1;
    if (i >= 0)
        users[i] = u;
    else
        users[nusers++] = u;
    return save_users();
}

int auth_user_del(const char *name)
{
    int i = user_index(name);
    if (i < 0)
        return -1;
    users[i] = users[--nusers];
    auth_session_drop_user(name);
    return save_users();
}

int auth_check(const char *name, const char *pass)
{
    refresh();
    const user_t *u = auth_user_find(name);
    uint8_t salt[16], dk[32];
    char h[65];
    if (!u) {
        /* same work as a real check, so the answer time does not reveal the account */
        memset(salt, 0, sizeof(salt));
        pbkdf2_sha256(pass, salt, sizeof(salt), PBKDF2_ITER, dk);
        return -1;
    }
    int sl = unhex(u->salt, salt, sizeof(salt));
    if (sl <= 0)
        return -1;
    pbkdf2_sha256(pass, salt, sl, u->iter, dk);
    hex(dk, sizeof(dk), h);
    return same(h, u->hash) ? 0 : -1;
}

/* ---------------------------------------------------------------- challenge-response */

static uint8_t fake_secret[32];
static int fake_ready;

int auth_random_hex(char *out, size_t nbytes)
{
    uint8_t b[64];
    if (nbytes > sizeof(b) || random_bytes(b, nbytes) < 0)
        return -1;
    hex(b, nbytes, out);
    return 0;
}

static int is_hex(const char *s, size_t len)
{
    if (strlen(s) != len)
        return 0;
    for (size_t i = 0; i < len; i++)
        if (!isxdigit((unsigned char)s[i]))
            return 0;
    return 1;
}

void auth_challenge(const char *name, char salt[33], int *iter)
{
    refresh();
    const user_t *u = auth_user_find(name);
    if (u) {
        snprintf(salt, 33, "%s", u->salt);
        *iter = u->iter;
        return;
    }
    /* stable per run and name, indistinguishable from a real salt */
    if (!fake_ready) {
        random_bytes(fake_secret, sizeof(fake_secret));
        fake_ready = 1;
    }
    char low[NAME_MAX_LEN + 1];
    size_t i = 0;
    for (; name[i] && i < NAME_MAX_LEN; i++)
        low[i] = (char)tolower((unsigned char)name[i]);
    low[i] = 0;
    uint8_t d[32];
    hmac_sha256(fake_secret, sizeof(fake_secret), (const uint8_t *)low, i, d);
    hex(d, 16, salt);
    *iter = PBKDF2_ITER;
}

int auth_verify(const char *name, const char *nonce_hex, const char *proof_hex)
{
    refresh();
    const user_t *u = auth_user_find(name);
    uint8_t nonce[32], dk[32], mac[32];
    char h[65];
    int nl = unhex(nonce_hex, nonce, sizeof(nonce));
    if (!u || nl <= 0 || !is_hex(proof_hex, 64) || unhex(u->hash, dk, sizeof(dk)) != 32)
        return -1;
    hmac_sha256(dk, sizeof(dk), nonce, nl, mac);
    hex(mac, sizeof(mac), h);
    char low[65];
    for (int i = 0; i < 65; i++)
        low[i] = (char)tolower((unsigned char)proof_hex[i]);
    return same(h, low) ? 0 : -1;
}

int auth_user_set_hash(const char *name, int role, const char *salt_hex, const char *hash_hex, int iter)
{
    if (!auth_valid_name(name) || role == ROLE_NONE || !is_hex(salt_hex, 32) || !is_hex(hash_hex, 64) ||
        iter < 1000 || iter > 1000000)
        return -1;
    refresh();
    int i = user_index(name);
    user_t u;
    if (i >= 0) {
        u = users[i];
    } else {
        if (nusers >= MAX_USERS)
            return -1;
        memset(&u, 0, sizeof(u));
        snprintf(u.name, sizeof(u.name), "%s", name);
    }
    u.role = role;
    u.iter = iter;
    for (int k = 0; k < 33; k++)
        u.salt[k] = (char)tolower((unsigned char)salt_hex[k]);
    for (int k = 0; k < 65; k++)
        u.hash[k] = (char)tolower((unsigned char)hash_hex[k]);
    if (i >= 0)
        users[i] = u;
    else
        users[nusers++] = u;
    return save_users();
}

/* ---------------------------------------------------------------- sessions */

static void token_hash(const char *token, char out[65])
{
    uint8_t d[32];
    sha256(token, strlen(token), d);
    hex(d, sizeof(d), out);
}

int auth_session_new(const char *name, char token[65])
{
    uint8_t raw[32];
    if (random_bytes(raw, sizeof(raw)) < 0)
        return -1;
    hex(raw, sizeof(raw), token);

    long now = time(NULL);
    int j = 0;
    for (int i = 0; i < nsessions; i++)
        if (sessions[i].expiry > now)
            sessions[j++] = sessions[i];
    nsessions = j;
    if (nsessions >= MAX_SESSIONS) {
        /* drop the oldest */
        memmove(&sessions[0], &sessions[1], (nsessions - 1) * sizeof(session_t));
        nsessions--;
    }
    session_t *s = &sessions[nsessions++];
    memset(s, 0, sizeof(*s));
    token_hash(token, s->hash);
    snprintf(s->name, sizeof(s->name), "%s", auth_user_find(name) ? auth_user_find(name)->name : name);
    s->expiry = now + SESSION_DAYS * 86400L;
    return save_sessions();
}

int auth_session_get(const char *token, char *name, size_t n)
{
    if (strlen(token) != 64)
        return -1;
    refresh();
    char h[65];
    token_hash(token, h);
    long now = time(NULL);
    for (int i = 0; i < nsessions; i++) {
        if (sessions[i].expiry > now && same(sessions[i].hash, h) && auth_user_find(sessions[i].name)) {
            snprintf(name, n, "%s", sessions[i].name);
            return 0;
        }
    }
    return -1;
}

void auth_session_drop(const char *token)
{
    char h[65];
    token_hash(token, h);
    for (int i = 0; i < nsessions; i++)
        if (same(sessions[i].hash, h)) {
            sessions[i] = sessions[--nsessions];
            save_sessions();
            return;
        }
}

void auth_session_drop_user(const char *name)
{
    int j = 0;
    for (int i = 0; i < nsessions; i++)
        if (strcasecmp(sessions[i].name, name))
            sessions[j++] = sessions[i];
    if (j != nsessions) {
        nsessions = j;
        save_sessions();
    }
}
