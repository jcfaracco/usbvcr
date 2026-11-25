#ifndef STREAM_PROVIDER_H
#define STREAM_PROVIDER_H

#include <stddef.h>
#include <stdint.h>
#include <glib.h>
#include "stream/mjpeg_parser.h"

typedef struct {
    mjpeg_stream_t *stream;
    char           *file_path;
    gboolean        loop;
    uint8_t         endpoint;          /* 0 = any IN endpoint, or e.g. 0x81 */
    size_t          current_frame;
    size_t          frame_offset;      /* byte offset within current frame */
    uint8_t         fid;               /* frame ID bit (0 or 1) */
    gboolean        active;            /* streaming active */
    uint32_t        frame_interval_us; /* default: 33333 us (~30 fps) */
} stream_provider_t;

stream_provider_t *stream_provider_new(const char *path, gboolean loop, uint8_t endpoint);
stream_provider_t *stream_provider_new_from_stream(mjpeg_stream_t *stream, gboolean loop, uint8_t endpoint);
void               stream_provider_set_active(stream_provider_t *p, gboolean active);
void               stream_provider_set_interval(stream_provider_t *p, uint32_t interval_us);
int                stream_provider_produce_packet(stream_provider_t *p, uint8_t ep, uint8_t *buf, size_t cap);
void               stream_provider_tick(stream_provider_t *p);
void               stream_provider_free(stream_provider_t *p);

#endif /* STREAM_PROVIDER_H */
