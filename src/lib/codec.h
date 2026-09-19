/*
 * codec.h – Generic table-driven item-stream encoder (README §6).
 *
 * Encodes symbolic descriptors (HID report items, etc.) into bytes using a
 * data table (codecs/*.yaml) loaded at runtime. No per-class C: new device
 * class = new table, dispatched by the table's `codec:` field.
 */

#ifndef CODEC_H
#define CODEC_H

#include <glib.h>

typedef struct Codec Codec;

/* Load a codec table (codecs/<name>.yaml). Returns NULL on error. */
Codec *codec_load(const char *table_path);
void   codec_free(Codec *codec);

/* Encode a symbolic item stream (a UsbNode-style key/value list) into `out`.
 * Unknown token => hard error (return -1); fuzzing needs determinism
 * (README §6.4). Returns 0 on success. */
int codec_encode(Codec *codec, void *item_stream, GByteArray *out);

#endif /* CODEC_H */
