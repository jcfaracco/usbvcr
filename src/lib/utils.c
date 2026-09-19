#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "utils.h"


/* Returns 1 if v1 > v2, -1 if v1 < v2, and 0 if v1 == v2 */
int cmp_versions(const char *v1, const char *v2) {
    const char *p1 = v1;
    const char *p2 = v2;

    while (*p1 || *p2) {
        long num1 = 0;
        long num2 = 0;

        if (*p1) {
            num1 = strtol(p1, (char **)&p1, 10);
        }
        if (*p2) {
            num2 = strtol(p2, (char **)&p2, 10);
        }

        if (num1 > num2) return 1;
        if (num1 < num2) return -1;

        // Skip the dot separator if present
        if (*p1 == '.') p1++;
        if (*p2 == '.') p2++;
    }

    return 0;
}


/* Return index of first string in `set` equal to `s`, else -1. */
int str_index_in_set(const char *s, const char *const set[], size_t n) {
    if (!s)
        return -1;
    for (size_t i = 0; i < n; i++)
        if (set[i] && strcmp(s, set[i]) == 0)
            return (int)i;
    return -1;
}


/* TODO: Check if there is an option instead of developing it. */
void trim_string(char *dst, const char *src, size_t dst_size) {
    size_t len = 0;

    while (isspace((unsigned char)*src))
        src++;

    while (src[len] != '\0' && src[len] != '#' && len < dst_size - 1) {
        len++;
    }

    while (len > 0 && isspace((unsigned char)src[len - 1]))
        len--;

    strncpy(dst, src, len);
    dst[len] = '\0';
}
