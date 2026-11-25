#include "stream/stream_provider.h"
#include "log.h"

#include <string.h>

stream_provider_t *stream_provider_new(const char *path, gboolean loop, uint8_t endpoint)
{
    stream_provider_t *p;
    mjpeg_stream_t *s;

    if (path == NULL)
    {
        return NULL;
    }

    s = mjpeg_stream_open(path);
    if (s == NULL)
    {
        return NULL;
    }

    p = stream_provider_new_from_stream(s, loop, endpoint);
    if (p != NULL)
    {
        p->file_path = g_strdup(path);
    }
    return p;
}

stream_provider_t *stream_provider_new_from_stream(mjpeg_stream_t *stream, gboolean loop, uint8_t endpoint)
{
    stream_provider_t *p;

    if (stream == NULL)
    {
        return NULL;
    }

    p = g_new0(stream_provider_t, 1);
    p->stream = stream;
    p->loop = loop;
    p->endpoint = endpoint;
    p->active = TRUE;
    p->fid = 0;
    p->frame_interval_us = 33333;   /* ~30 FPS */

    return p;
}

void stream_provider_set_active(stream_provider_t *p, gboolean active)
{
    if (p == NULL)
    {
        return;
    }

    if (active && !p->active)
    {
        p->current_frame = 0;
        p->frame_offset = 0;
        p->fid = 0;
    }
    p->active = active;
}

void stream_provider_set_interval(stream_provider_t *p, uint32_t interval_us)
{
    if (p == NULL || interval_us == 0)
    {
        return;
    }
    p->frame_interval_us = interval_us;
}

int stream_provider_produce_packet(stream_provider_t *p, uint8_t ep, uint8_t *buf, size_t cap)
{
    const uint8_t *frame_data;
    size_t frame_len = 0;
    size_t max_payload;
    size_t remaining;
    size_t slice;
    gboolean is_eof;

    if (p == NULL || !p->active || buf == NULL || cap < 3)
    {
        return 0;
    }

    /* If provider was bound to a specific endpoint, only handle matches */
    if (p->endpoint != 0 && p->endpoint != ep)
    {
        return -1;
    }

    if (p->stream == NULL || p->stream->frame_count == 0)
    {
        return 0;
    }

    frame_data = mjpeg_stream_get_frame(p->stream, p->current_frame, &frame_len);
    if (frame_data == NULL || frame_len == 0)
    {
        if (p->loop && p->stream->frame_count > 0)
        {
            p->current_frame = 0;
            p->frame_offset = 0;
            frame_data = mjpeg_stream_get_frame(p->stream, p->current_frame, &frame_len);
        }
        if (frame_data == NULL || frame_len == 0)
        {
            return 0;
        }
    }

    max_payload = cap - 2;
    remaining = frame_len - p->frame_offset;
    slice = remaining < max_payload ? remaining : max_payload;
    is_eof = (p->frame_offset + slice >= frame_len);

    /* 2-byte UVC Payload Header */
    buf[0] = 0x02;                                 /* bHeaderLength */
    buf[1] = p->fid | (is_eof ? 0x02 : 0x00);      /* FID (bit 0) | EOF (bit 1) */

    memcpy(buf + 2, frame_data + p->frame_offset, slice);

    p->frame_offset += slice;
    if (is_eof)
    {
        p->fid ^= 1;
        p->frame_offset = 0;
        p->current_frame++;
        if (p->current_frame >= p->stream->frame_count)
        {
            if (p->loop)
            {
                p->current_frame = 0;
            }
            else
            {
                p->active = FALSE;
            }
        }
    }

    return (int)(slice + 2);
}

void stream_provider_tick(stream_provider_t *p)
{
    (void)p;
}

void stream_provider_free(stream_provider_t *p)
{
    if (p == NULL)
    {
        return;
    }

    mjpeg_stream_free(p->stream);
    g_free(p->file_path);
    g_free(p);
}
