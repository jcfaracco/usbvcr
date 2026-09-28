#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include <sys/socket.h>

#include "record.h"
#include "transport.h"
#include "protocol.h"
#include "log.h"

static FILE *g_fp;

int record_open(const char *path)
{
    g_fp = fopen(path, "wb");
    if (!g_fp) {
        LOG_ERR("record: cannot open %s", path);
        return -1;
    }
    transport_set_record(g_fp);
    LOG_INFO("Recording frames to %s", path);
    return 0;
}

void record_close(void)
{
    transport_set_record(NULL);
    if (g_fp) {
        fclose(g_fp);
        g_fp = NULL;
    }
}

int replay_run(int sock, const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        LOG_ERR("replay: cannot open %s", path);
        return -1;
    }

    uint8_t frame[USB_VCR_BUF_SIZE];
    uint32_t len;
    size_t count = 0;

    while (fread(&len, sizeof(len), 1, fp) == 1) {
        if (len > sizeof(frame)) {
            LOG_ERR("replay: frame %zu too large (%u bytes)", count, len);
            fclose(fp);
            return -1;
        }
        if (fread(frame, 1, len, fp) != len) {
            LOG_ERR("replay: truncated frame %zu", count);
            fclose(fp);
            return -1;
        }
        if (send(sock, frame, len, 0) < 0) {
            perror("send");
            fclose(fp);
            return -1;
        }
        count++;
    }

    fclose(fp);
    LOG_INFO("Replayed %zu frames from %s", count, path);
    return 0;
}
