#include "storage/disk_provider.h"
#include "log.h"

#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

struct disk_provider {
    char     *file_path;
    int       fd;
    uint64_t  disk_size;
    uint32_t  block_size;
    uint64_t  total_blocks;
    uint8_t   ep_in;
    uint8_t   ep_out;

    /* Pending WRITE (10) state */
    uint32_t  pending_write_lba;
    uint32_t  pending_write_bytes_total;
    uint32_t  pending_write_bytes_received;
    uint8_t   pending_write_tag[4];
    gboolean  has_pending_write;
};

static void queue_csw(GQueue *in_queue, const uint8_t *tag, uint32_t residue, uint8_t status)
{
    if (in_queue == NULL)
        return;

    GByteArray *csw = g_byte_array_sized_new(13);
    uint8_t csw_buf[13];
    memcpy(csw_buf, "USBS", 4);
    memcpy(csw_buf + 4, tag, 4);
    csw_buf[8] = (uint8_t)(residue & 0xff);
    csw_buf[9] = (uint8_t)((residue >> 8) & 0xff);
    csw_buf[10] = (uint8_t)((residue >> 16) & 0xff);
    csw_buf[11] = (uint8_t)((residue >> 24) & 0xff);
    csw_buf[12] = status;

    g_byte_array_append(csw, csw_buf, 13);
    g_queue_push_tail(in_queue, csw);
}

static void queue_in_payload(GQueue *in_queue, const uint8_t *data, size_t len)
{
    if (in_queue == NULL || data == NULL || len == 0)
        return;

    GByteArray *arr = g_byte_array_sized_new(len);
    g_byte_array_append(arr, data, len);
    g_queue_push_tail(in_queue, arr);
}

disk_provider_t *disk_provider_new(const char *path, uint8_t ep_in, uint8_t ep_out)
{
    if (path == NULL)
        return NULL;

    int fd = open(path, O_RDWR);
    if (fd < 0) {
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            LOG_ERR("disk_provider: failed to open '%s'", path);
            return NULL;
        }
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        LOG_ERR("disk_provider: failed to stat '%s'", path);
        close(fd);
        return NULL;
    }

    disk_provider_t *dp = g_new0(disk_provider_t, 1);
    dp->file_path = g_strdup(path);
    dp->fd = fd;
    dp->disk_size = (uint64_t)st.st_size;
    dp->block_size = 512;
    dp->total_blocks = dp->disk_size / dp->block_size;
    dp->ep_in = (ep_in != 0) ? ep_in : 0x81;
    dp->ep_out = (ep_out != 0) ? ep_out : 0x02;

    LOG_INFO("disk_provider: attached '%s' size=%" G_GUINT64_FORMAT " bytes (%lu blocks)",
             path, dp->disk_size, (unsigned long)dp->total_blocks);

    return dp;
}

void disk_provider_free(disk_provider_t *dp)
{
    if (dp == NULL)
        return;

    if (dp->fd >= 0) {
        close(dp->fd);
        dp->fd = -1;
    }
    g_free(dp->file_path);
    g_free(dp);
}

uint8_t disk_provider_get_ep_in(disk_provider_t *dp)
{
    return dp ? dp->ep_in : 0x81;
}

uint8_t disk_provider_get_ep_out(disk_provider_t *dp)
{
    return dp ? dp->ep_out : 0x02;
}

int disk_provider_handle_out(disk_provider_t *dp, const uint8_t *buf, size_t len,
                             GQueue *in_queue)
{
    if (dp == NULL || buf == NULL || in_queue == NULL)
        return -1;

    /* Check if this OUT packet is write data payload for a pending WRITE (10) */
    if (dp->has_pending_write) {
        off_t offset = (off_t)dp->pending_write_lba * dp->block_size +
                       dp->pending_write_bytes_received;
        ssize_t written = pwrite(dp->fd, buf, len, offset);
        if (written > 0)
            dp->pending_write_bytes_received += (uint32_t)written;

        if (dp->pending_write_bytes_received >= dp->pending_write_bytes_total) {
            dp->has_pending_write = FALSE;
            queue_csw(in_queue, dp->pending_write_tag, 0, 0);
        }
        return 0;
    }

    /* Check for 31-byte MSC Command Block Wrapper ("USBC") */
    if (len < 31 || memcmp(buf, "USBC", 4) != 0)
        return -1;

    const uint8_t *tag = buf + 4;
    uint32_t transfer_len = (uint32_t)buf[8] |
                            ((uint32_t)buf[9] << 8) |
                            ((uint32_t)buf[10] << 16) |
                            ((uint32_t)buf[11] << 24);
    uint8_t cdb_len = buf[14];
    const uint8_t *cdb = buf + 15;

    if (cdb_len == 0) {
        queue_csw(in_queue, tag, transfer_len, 1);
        return 0;
    }

    uint8_t opcode = cdb[0];

    switch (opcode) {
    case 0x12: { /* INQUIRY */
        uint8_t inq[36] = {
            0x00, /* Direct access block device */
            0x80, /* Removable */
            0x02, /* ANSI SCSI-2 */
            0x02, /* Response format */
            0x1f, /* Additional length (36 - 5 = 31) */
            0x00, 0x00, 0x00,
            'S', 'a', 'n', 'D', 'i', 's', 'k', ' ',
            'C', 'r', 'u', 'z', 'e', 'r', ' ', 'B',
            'l', 'a', 'd', 'e', ' ', ' ', ' ', ' ',
            '1', '.', '0', '0'
        };
        size_t send_len = sizeof(inq);
        if (transfer_len < send_len)
            send_len = transfer_len;

        queue_in_payload(in_queue, inq, send_len);
        uint32_t residue = transfer_len > send_len ? (transfer_len - (uint32_t)send_len) : 0;
        queue_csw(in_queue, tag, residue, 0);
        break;
    }

    case 0x00: { /* TEST UNIT READY */
        queue_csw(in_queue, tag, 0, 0);
        break;
    }

    case 0x25: { /* READ CAPACITY (10) */
        uint32_t last_lba = dp->total_blocks > 0 ? (uint32_t)(dp->total_blocks - 1) : 0;
        uint32_t blk_size = dp->block_size;
        uint8_t cap_resp[8] = {
            (uint8_t)(last_lba >> 24),
            (uint8_t)(last_lba >> 16),
            (uint8_t)(last_lba >> 8),
            (uint8_t)(last_lba & 0xff),
            (uint8_t)(blk_size >> 24),
            (uint8_t)(blk_size >> 16),
            (uint8_t)(blk_size >> 8),
            (uint8_t)(blk_size & 0xff)
        };
        queue_in_payload(in_queue, cap_resp, sizeof(cap_resp));
        queue_csw(in_queue, tag, 0, 0);
        break;
    }

    case 0x9E: { /* SERVICE ACTION IN (16) / READ CAPACITY (16) */
        if ((cdb[1] & 0x1f) == 0x10) {
            uint64_t last_lba = dp->total_blocks > 0 ? (dp->total_blocks - 1) : 0;
            uint32_t blk_size = dp->block_size;
            uint8_t cap16[32];
            memset(cap16, 0, sizeof(cap16));
            for (int i = 0; i < 8; i++)
                cap16[i] = (uint8_t)((last_lba >> (56 - i * 8)) & 0xff);
            cap16[8] = (uint8_t)(blk_size >> 24);
            cap16[9] = (uint8_t)(blk_size >> 16);
            cap16[10] = (uint8_t)(blk_size >> 8);
            cap16[11] = (uint8_t)(blk_size & 0xff);

            size_t send_len = sizeof(cap16);
            if (transfer_len < send_len)
                send_len = transfer_len;
            queue_in_payload(in_queue, cap16, send_len);
            queue_csw(in_queue, tag, 0, 0);
        } else {
            queue_csw(in_queue, tag, transfer_len, 1);
        }
        break;
    }

    case 0x1A: { /* MODE SENSE (6) */
        /* 4-byte header: not write-protected */
        uint8_t mode[4] = { 0x03, 0x00, 0x00, 0x00 };
        size_t send_len = sizeof(mode);
        if (transfer_len < send_len)
            send_len = transfer_len;
        queue_in_payload(in_queue, mode, send_len);
        queue_csw(in_queue, tag, 0, 0);
        break;
    }

    case 0x5A: { /* MODE SENSE (10) */
        uint8_t mode[8] = { 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
        size_t send_len = sizeof(mode);
        if (transfer_len < send_len)
            send_len = transfer_len;
        queue_in_payload(in_queue, mode, send_len);
        queue_csw(in_queue, tag, 0, 0);
        break;
    }

    case 0x23: { /* READ FORMAT CAPACITIES */
        uint32_t blocks = (uint32_t)dp->total_blocks;
        uint32_t blk_size = dp->block_size;
        uint8_t rfc[12] = {
            0x00, 0x00, 0x00, 0x08, /* List Length 8 */
            (uint8_t)(blocks >> 24),
            (uint8_t)(blocks >> 16),
            (uint8_t)(blocks >> 8),
            (uint8_t)(blocks & 0xff),
            0x02, /* Formatted Media */
            (uint8_t)(blk_size >> 16),
            (uint8_t)(blk_size >> 8),
            (uint8_t)(blk_size & 0xff)
        };
        size_t send_len = sizeof(rfc);
        if (transfer_len < send_len)
            send_len = transfer_len;
        queue_in_payload(in_queue, rfc, send_len);
        queue_csw(in_queue, tag, 0, 0);
        break;
    }

    case 0x28: { /* READ (10) */
        uint32_t lba = ((uint32_t)cdb[2] << 24) |
                       ((uint32_t)cdb[3] << 16) |
                       ((uint32_t)cdb[4] << 8) |
                       (uint32_t)cdb[5];
        uint16_t blks = ((uint16_t)cdb[7] << 8) | (uint16_t)cdb[8];
        size_t req_bytes = (size_t)blks * dp->block_size;

        uint8_t *read_buf = g_malloc0(req_bytes > 0 ? req_bytes : 512);
        if (req_bytes > 0 && dp->fd >= 0) {
            off_t offset = (off_t)lba * dp->block_size;
            ssize_t n = pread(dp->fd, read_buf, req_bytes, offset);
            if (n < 0)
                memset(read_buf, 0, req_bytes);
        }

        queue_in_payload(in_queue, read_buf, req_bytes);
        g_free(read_buf);
        queue_csw(in_queue, tag, 0, 0);
        break;
    }

    case 0x2A: { /* WRITE (10) */
        uint32_t lba = ((uint32_t)cdb[2] << 24) |
                       ((uint32_t)cdb[3] << 16) |
                       ((uint32_t)cdb[4] << 8) |
                       (uint32_t)cdb[5];
        uint16_t blks = ((uint16_t)cdb[7] << 8) | (uint16_t)cdb[8];
        size_t req_bytes = (size_t)blks * dp->block_size;

        if (req_bytes == 0) {
            queue_csw(in_queue, tag, 0, 0);
        } else {
            dp->pending_write_lba = lba;
            dp->pending_write_bytes_total = (uint32_t)req_bytes;
            dp->pending_write_bytes_received = 0;
            memcpy(dp->pending_write_tag, tag, 4);
            dp->has_pending_write = TRUE;
        }
        break;
    }

    case 0x35: { /* SYNCHRONIZE CACHE (10) */
        if (dp->fd >= 0)
            fdatasync(dp->fd);
        queue_csw(in_queue, tag, 0, 0);
        break;
    }

    case 0x1E: { /* PREVENT ALLOW MEDIUM REMOVAL */
        queue_csw(in_queue, tag, 0, 0);
        break;
    }

    case 0x03: { /* REQUEST SENSE */
        uint8_t sense[18] = {
            0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00
        };
        size_t send_len = sizeof(sense);
        if (transfer_len < send_len)
            send_len = transfer_len;
        queue_in_payload(in_queue, sense, send_len);
        queue_csw(in_queue, tag, 0, 0);
        break;
    }

    default:
        LOG_WARN("disk_provider: unhandled SCSI opcode 0x%02x", opcode);
        queue_csw(in_queue, tag, transfer_len, 1);
        break;
    }

    return 0;
}
