#ifndef VIDEO_PARSER_H
#define VIDEO_PARSER_H

#include <stddef.h>
#include <stdint.h>
#include <glib.h>

typedef enum {
    VIDEO_FORMAT_AUTO = 0,
    VIDEO_FORMAT_MJPEG,
    VIDEO_FORMAT_YUY2,
    VIDEO_FORMAT_RAW,
} video_format_t;

typedef struct {
    size_t offset;
    size_t length;
} video_frame_t;

typedef struct {
    char          *path;
    video_format_t format;
    GMappedFile   *mapped_file;
    const uint8_t *data;
    size_t         data_len;
    video_frame_t *frames;
    size_t         frame_count;
    gboolean       owns_data;
} video_stream_t;

video_stream_t *video_stream_open(const char *path);
video_stream_t *video_stream_open_with_format(const char *path, video_format_t format);
video_stream_t *video_stream_new_from_bytes(const uint8_t *data, size_t len, video_format_t format);
const uint8_t  *video_stream_get_frame(const video_stream_t *stream, size_t index, size_t *out_len);
void            video_stream_free(video_stream_t *stream);

/* Aliases for backwards compatibility */
typedef video_frame_t  mjpeg_frame_t;
typedef video_stream_t mjpeg_stream_t;

#define mjpeg_stream_open(path) video_stream_open(path)
#define mjpeg_stream_new_from_bytes(data, len) video_stream_new_from_bytes((data), (len), VIDEO_FORMAT_MJPEG)
#define mjpeg_stream_get_frame(stream, index, out_len) video_stream_get_frame((stream), (index), (out_len))
#define mjpeg_stream_free(stream) video_stream_free(stream)

#endif /* VIDEO_PARSER_H */
