#ifndef __UTILS_H_
#define __UTILS_H_

#include <stddef.h>

int cmp_versions(const char *v1, const char *v2);

void trim_string(char *dst, const char *src, size_t dst_size);

/* Return index of first string in `set` equal to `s` (strcmp), else -1.
 * `set` has `n` entries. */
int str_index_in_set(const char *s, const char *const set[], size_t n);

#endif /* __UTILS_H_ */
