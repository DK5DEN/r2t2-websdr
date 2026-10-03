#ifndef AUTH_H
#define AUTH_H

#include <stddef.h>

/* Roles, ordered: each one includes the rights of the ones below. */
enum { ROLE_NONE, ROLE_USER, ROLE_EDITOR, ROLE_ADMIN };

#define MAX_USERS     64
#define NAME_MAX_LEN  24

typedef struct {
    char name[NAME_MAX_LEN + 1];
    int role;
    int iter;
    char salt[33];      /* hex */
    char hash[65];      /* hex */
} user_t;

int auth_init(const char *dir);

const char *role_name(int role);
int role_parse(const char *s);
int auth_valid_name(const char *name);

int auth_user_count(void);
int auth_admin_count(void);
const user_t *auth_user_at(int i);
const user_t *auth_user_find(const char *name);
int auth_user_set(const char *name, int role, const char *pass);
int auth_user_del(const char *name);
int auth_check(const char *name, const char *pass);

/*
 * Login without the password on the wire (challenge-response):
 *   server -> salt, iterations, nonce
 *   client -> proof = HMAC-SHA256(PBKDF2(password, salt, iterations), nonce)
 * Unknown names get a stable fake salt, so the answer does not reveal accounts.
 */
#define AUTH_ITER 10000
int auth_random_hex(char *out, size_t nbytes);
void auth_challenge(const char *name, char salt[33], int *iter);
int auth_verify(const char *name, const char *nonce_hex, const char *proof_hex);
/* store a password the browser already hashed (salt and PBKDF2 result in hex) */
int auth_user_set_hash(const char *name, int role, const char *salt_hex, const char *hash_hex, int iter);

int auth_session_new(const char *name, char token[65]);
int auth_session_get(const char *token, char *name, size_t n);
void auth_session_drop(const char *token);
void auth_session_drop_user(const char *name);

#endif
