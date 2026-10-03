#ifndef SHA1_H
#define SHA1_H

#include <stddef.h>
#include <stdint.h>

void sha1(const void *data, size_t len, uint8_t out[20]);
void base64(const uint8_t *in, size_t len, char *out);

#endif
