#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>

int cmp_versions(const char *v1, const char *v2);

void trim_string(char *dst, const char *src, size_t dst_size);

/* Return index of first string in `set` equal to `s` (strcmp), else -1.
 * `set` has `n` entries. */
int str_index_in_set(const char *s, const char *const set[], size_t n);

/* Return allocated path to root of usbvcr data/ directory, or NULL. Caller frees. */
char *usbvcr_get_data_dir(void);

/* Search for file in data directory under optional subdir. Caller frees. */
char *usbvcr_find_data_file(const char *subdir, const char *filename);

#endif /* UTILS_H */
