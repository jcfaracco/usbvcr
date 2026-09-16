#include <string.h>
#include <ctype.h>

#include "utils.h"


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
