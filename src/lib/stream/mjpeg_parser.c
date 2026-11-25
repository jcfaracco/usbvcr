#include "stream/mjpeg_parser.h"
#include "log.h"

#include <string.h>

static void mjpeg_parse_frames(mjpeg_stream_t *s)
{
    GArray *frames;
    const uint8_t *buf;
    size_t len;
    size_t i;

    frames = g_array_new(FALSE, FALSE, sizeof(mjpeg_frame_t));
    buf = s->data;
    len = s->data_len;
    i = 0;

    while (i + 1 < len)
    {
        /* Look for SOI marker: 0xFF 0xD8 */
        if (buf[i] == 0xFF && buf[i + 1] == 0xD8)
        {
            size_t start = i;
            size_t j = i + 2;
            gboolean found_eoi = FALSE;

            /* Search for matching EOI marker: 0xFF 0xD9 */
            while (j + 1 < len)
            {
                if (buf[j] == 0xFF && buf[j + 1] == 0xD9)
                {
                    mjpeg_frame_t frame;
                    frame.offset = start;
                    frame.length = (j + 2) - start;
                    g_array_append_val(frames, frame);
                    found_eoi = TRUE;
                    i = j + 2;
                    break;
                }
                j++;
            }

            if (!found_eoi)
            {
                /* Partial or trailing frame up to EOF */
                mjpeg_frame_t frame;
                frame.offset = start;
                frame.length = len - start;
                g_array_append_val(frames, frame);
                break;
            }
        }
        else
        {
            i++;
        }
    }

    /* Fallback: if no SOI/EOI markers found, treat entire buffer as 1 frame */
    if (frames->len == 0 && len > 0)
    {
        mjpeg_frame_t frame;
        frame.offset = 0;
        frame.length = len;
        g_array_append_val(frames, frame);
    }

    s->frame_count = frames->len;
    s->frames = (mjpeg_frame_t *)g_array_free(frames, FALSE);
}

mjpeg_stream_t *mjpeg_stream_open(const char *path)
{
    mjpeg_stream_t *s;
    GMappedFile *mf;
    GError *err = NULL;

    if (path == NULL)
    {
        return NULL;
    }

    mf = g_mapped_file_new(path, FALSE, &err);
    if (mf == NULL)
    {
        LOG_WARN("mjpeg_parser: unable to map file '%s': %s", path,
                 err ? err->message : "unknown error");
        if (err != NULL)
        {
            g_error_free(err);
        }
        return NULL;
    }

    s = g_new0(mjpeg_stream_t, 1);
    s->path = g_strdup(path);
    s->mapped_file = mf;
    s->data = (const uint8_t *)g_mapped_file_get_contents(mf);
    s->data_len = g_mapped_file_get_length(mf);

    mjpeg_parse_frames(s);
    return s;
}

mjpeg_stream_t *mjpeg_stream_new_from_bytes(const uint8_t *data, size_t len)
{
    mjpeg_stream_t *s;

    if (data == NULL || len == 0)
    {
        return NULL;
    }

    s = g_new0(mjpeg_stream_t, 1);
    s->data = g_memdup2(data, len);
    s->data_len = len;
    s->owns_data = TRUE;

    mjpeg_parse_frames(s);
    return s;
}

const uint8_t *mjpeg_stream_get_frame(const mjpeg_stream_t *stream, size_t index, size_t *out_len)
{
    if (stream == NULL || index >= stream->frame_count || stream->frames == NULL)
    {
        if (out_len != NULL)
        {
            *out_len = 0;
        }
        return NULL;
    }

    if (out_len != NULL)
    {
        *out_len = stream->frames[index].length;
    }
    return stream->data + stream->frames[index].offset;
}

void mjpeg_stream_free(mjpeg_stream_t *stream)
{
    if (stream == NULL)
    {
        return;
    }

    g_free(stream->frames);
    g_free(stream->path);

    if (stream->mapped_file != NULL)
    {
        g_mapped_file_unref(stream->mapped_file);
    }
    else if (stream->owns_data && stream->data != NULL)
    {
        g_free((void *)stream->data);
    }

    g_free(stream);
}
