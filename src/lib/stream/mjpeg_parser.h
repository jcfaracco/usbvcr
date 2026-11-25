#ifndef MJPEG_PARSER_H
#define MJPEG_PARSER_H

#include <stddef.h>
#include <stdint.h>
#include <glib.h>

typedef struct {
    size_t offset;
    size_t length;
} mjpeg_frame_t;

typedef struct {
    char          *path;
    GMappedFile   *mapped_file;
    const uint8_t *data;
    size_t         data_len;
    mjpeg_frame_t *frames;
    size_t         frame_count;
    gboolean       owns_data;
} mjpeg_stream_t;

mjpeg_stream_t *mjpeg_stream_open(const char *path);
mjpeg_stream_t *mjpeg_stream_new_from_bytes(const uint8_t *data, size_t len);
const uint8_t  *mjpeg_stream_get_frame(const mjpeg_stream_t *stream, size_t index, size_t *out_len);
void            mjpeg_stream_free(mjpeg_stream_t *stream);

#endif /* MJPEG_PARSER_H */
