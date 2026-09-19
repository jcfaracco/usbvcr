/*
 * codec.c – Generic table-driven encoder.  STUB (README §9 build step 3).
 *
 * TODO (README §6.2 encoder contract):
 *   1. lookup key in tokens -> (type, tag, width, symbol-map, flagset)
 *   2. resolve value: number as-is; string via symbols[key];
 *      comma-list OR-folded via flagsets
 *   3. width:auto -> pick 0/1/2/4 bytes by magnitude -> bSize code
 *   4. emit prefix (tag<<4 | type<<2 | bSize) then operand little-endian
 */

#include "codec.h"

#include <stdlib.h>
#include <glib.h>

struct codec {
    char *codec_name; /* value of table `codec:` field */
    /* TODO: tokens / symbols / flagsets tables */
};

codec_t *codec_load(const char *table_path)
{
    (void)table_path;
    return NULL; /* not implemented */
}

void codec_free(codec_t *codec)
{
    if (!codec)
        return;
    g_free(codec->codec_name);
    g_free(codec);
}

int codec_encode(codec_t *codec, void *item_stream, GByteArray *out)
{
    (void)codec;
    (void)item_stream;
    (void)out;
    return -1; /* not implemented */
}
