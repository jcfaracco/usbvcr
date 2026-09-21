/*
 * codec.h – Generic table-driven item-stream encoder (README §6).
 *
 * Encodes symbolic descriptors (HID report items, etc.) into bytes using a
 * data table (codecs/\*.yaml) loaded at runtime. No per-class C: new device
 * class = new table, dispatched by the table's `codec:` field.
 */

#ifndef CODEC_H
#define CODEC_H

#include <glib.h>

typedef struct codec codec_t;

/* One symbolic item to encode: a token name and its raw scalar text (a number,
 * a symbol name, or a comma-separated flag list). Both borrowed. */
typedef struct {
    const char *key;
    const char *value;
} codec_item_t;

/* Load a codec table (codecs/<name>.yaml). Returns NULL on error. */
codec_t *codec_load(const char *table_path);
void     codec_free(codec_t *codec);

/* The table's `codec:` name (dispatch key), borrowed from `codec`. */
const char *codec_name(const codec_t *codec);

/* Encode an ordered item stream (GList of codec_item_t*) into `out`.
 * Unknown token / symbol / flag => hard error (return -1); fuzzing needs
 * determinism (README §6.4). Returns 0 on success. */
int codec_encode(codec_t *codec, const GList *items, GByteArray *out);

#endif /* CODEC_H */
