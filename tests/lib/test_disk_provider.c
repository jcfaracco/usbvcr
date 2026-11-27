/* Unit tests for disk provider (src/lib/storage/disk_provider.c). */

#include <check.h>
#include <fcntl.h>
#include <glib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "bytes.h"

/* Mock control flags */
static gboolean mock_fstat_fail = FALSE;
static gboolean mock_pread_fail = FALSE;
static gboolean mock_pwrite_zero = FALSE;

static inline int mock_fstat(int fd, struct stat *buf)
{
    if (mock_fstat_fail)
    {
        return -1;
    }
    return fstat(fd, buf);
}
#define fstat mock_fstat

static inline ssize_t mock_pread(int fd, void *buf, size_t count, off_t offset)
{
    if (mock_pread_fail)
    {
        return -1;
    }
    return pread(fd, buf, count, offset);
}
#define pread mock_pread

static inline ssize_t mock_pwrite(int fd, const void *buf, size_t count, off_t offset)
{
    if (mock_pwrite_zero)
    {
        return 0;
    }
    return pwrite(fd, buf, count, offset);
}
#define pwrite mock_pwrite

#define static
#include "storage/disk_provider.c"
#undef static
#undef fstat
#undef pread
#undef pwrite

static void free_queue_item(gpointer data)
{
    g_byte_array_unref((GByteArray *)data);
}

static void build_cbw(uint8_t *cbw, uint32_t tag, uint32_t transfer_len, uint8_t flags,
                      uint8_t cdb_len, const uint8_t *cdb)
{
    memset(cbw, 0, 31);
    memcpy(cbw, "USBC", 4);
    put_u32le(cbw + 4, tag);
    put_u32le(cbw + 8, transfer_len);
    cbw[12] = flags;
    cbw[13] = 0; /* LUN */
    cbw[14] = cdb_len;
    if (cdb_len > 0 && cdb != NULL)
    {
        memcpy(cbw + 15, cdb, cdb_len <= 16 ? cdb_len : 16);
    }
}

START_TEST(test_disk_provider_new_and_properties)
{
    char tmp_path[] = "/tmp/usbvcr_disk_test_XXXXXX";
    int fd = mkstemp(tmp_path);
    ck_assert_int_ge(fd, 0);

    /* Write 2048 bytes (4 blocks of 512) */
    uint8_t init_data[2048];
    memset(init_data, 0xAA, sizeof(init_data));
    ck_assert_int_eq(write(fd, init_data, sizeof(init_data)), sizeof(init_data));
    close(fd);

    /* NULL path */
    ck_assert_ptr_null(disk_provider_new(NULL, 0x81, 0x02));

    /* Non-existent path */
    ck_assert_ptr_null(disk_provider_new("/nonexistent/file/path", 0x81, 0x02));

    /* Read-only file fallback to open O_RDONLY */
    ck_assert_int_eq(chmod(tmp_path, 0444), 0);
    disk_provider_t *dp_ro = disk_provider_new(tmp_path, 0, 0);
    ck_assert_ptr_nonnull(dp_ro);
    ck_assert_int_eq(disk_provider_get_ep_in(dp_ro), 0x81);
    ck_assert_int_eq(disk_provider_get_ep_out(dp_ro), 0x02);
    disk_provider_free(dp_ro);

    /* Restore write permission */
    ck_assert_int_eq(chmod(tmp_path, 0644), 0);

    /* Test fstat failure */
    mock_fstat_fail = TRUE;
    ck_assert_ptr_null(disk_provider_new(tmp_path, 0x83, 0x03));
    mock_fstat_fail = FALSE;

    /* Successful open with custom endpoints */
    disk_provider_t *dp = disk_provider_new(tmp_path, 0x85, 0x05);
    ck_assert_ptr_nonnull(dp);
    ck_assert_int_eq(disk_provider_get_ep_in(dp), 0x85);
    ck_assert_int_eq(disk_provider_get_ep_out(dp), 0x05);

    /* NULL getters */
    ck_assert_int_eq(disk_provider_get_ep_in(NULL), 0x81);
    ck_assert_int_eq(disk_provider_get_ep_out(NULL), 0x02);

    disk_provider_free(dp);
    disk_provider_free(NULL);

    unlink(tmp_path);
}
END_TEST

START_TEST(test_disk_provider_queue_helpers)
{
    GQueue *queue = g_queue_new();
    uint8_t tag[4] = { 1, 2, 3, 4 };
    uint8_t data[10] = { 0 };

    /* queue_csw with NULL queue */
    queue_csw(NULL, tag, 0, 0);

    /* queue_in_payload with NULL queue, NULL data, 0 length */
    queue_in_payload(NULL, data, sizeof(data));
    queue_in_payload(queue, NULL, sizeof(data));
    queue_in_payload(queue, data, 0);
    ck_assert_int_eq(g_queue_get_length(queue), 0);

    /* Valid payload */
    queue_in_payload(queue, data, 5);
    ck_assert_int_eq(g_queue_get_length(queue), 1);

    g_queue_free_full(queue, free_queue_item);
}
END_TEST

START_TEST(test_disk_provider_handle_out_invalid)
{
    char tmp_path[] = "/tmp/usbvcr_disk_test_XXXXXX";
    int fd = mkstemp(tmp_path);
    ck_assert_int_ge(fd, 0);
    uint8_t dummy[1024] = { 0 };
    ck_assert_int_eq(write(fd, dummy, sizeof(dummy)), sizeof(dummy));
    close(fd);

    disk_provider_t *dp = disk_provider_new(tmp_path, 0, 0);
    ck_assert_ptr_nonnull(dp);

    GQueue *queue = g_queue_new();
    uint8_t buf[64];

    /* NULL parameters */
    ck_assert_int_eq(disk_provider_handle_out(NULL, buf, sizeof(buf), queue), -1);
    ck_assert_int_eq(disk_provider_handle_out(dp, NULL, sizeof(buf), queue), -1);
    ck_assert_int_eq(disk_provider_handle_out(dp, buf, sizeof(buf), NULL), -1);

    /* Length < 31 */
    ck_assert_int_eq(disk_provider_handle_out(dp, buf, 30, queue), -1);

    /* Invalid signature */
    memcpy(buf, "XXXX", 4);
    ck_assert_int_eq(disk_provider_handle_out(dp, buf, 31, queue), -1);

    /* Valid USBC signature but cdb_len == 0 */
    build_cbw(buf, 0x1234, 0, 0, 0, NULL);
    ck_assert_int_eq(disk_provider_handle_out(dp, buf, 31, queue), 0);
    ck_assert_int_eq(g_queue_get_length(queue), 1); /* CSW returned */

    g_queue_free_full(queue, free_queue_item);
    disk_provider_free(dp);
    unlink(tmp_path);
}
END_TEST

START_TEST(test_disk_provider_scsi_commands)
{
    char tmp_path[] = "/tmp/usbvcr_disk_test_XXXXXX";
    int fd = mkstemp(tmp_path);
    ck_assert_int_ge(fd, 0);
    uint8_t dummy[2048];
    for (size_t i = 0; i < sizeof(dummy); i++)
    {
        dummy[i] = (uint8_t)(i & 0xff);
    }
    ck_assert_int_eq(write(fd, dummy, sizeof(dummy)), sizeof(dummy));
    close(fd);

    disk_provider_t *dp = disk_provider_new(tmp_path, 0, 0);
    ck_assert_ptr_nonnull(dp);

    GQueue *queue = g_queue_new();
    uint8_t cbw[31];
    uint8_t cdb[16];

    /* 1. TEST UNIT READY (0x00) */
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x00;
    build_cbw(cbw, 0x1, 0, 0, 6, cdb);
    ck_assert_int_eq(disk_provider_handle_out(dp, cbw, sizeof(cbw), queue), 0);
    ck_assert_int_eq(g_queue_get_length(queue), 1);
    g_queue_free_full(queue, free_queue_item);
    queue = g_queue_new();

    /* 2. INQUIRY (0x12) - normal & truncated */
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x12;
    build_cbw(cbw, 0x2, 36, 0x80, 6, cdb);
    ck_assert_int_eq(disk_provider_handle_out(dp, cbw, sizeof(cbw), queue), 0);
    ck_assert_int_eq(g_queue_get_length(queue), 2); /* payload + CSW */
    g_queue_free_full(queue, free_queue_item);
    queue = g_queue_new();

    build_cbw(cbw, 0x3, 10, 0x80, 6, cdb); /* transfer_len < send_len */
    ck_assert_int_eq(disk_provider_handle_out(dp, cbw, sizeof(cbw), queue), 0);
    ck_assert_int_eq(g_queue_get_length(queue), 2);
    g_queue_free_full(queue, free_queue_item);
    queue = g_queue_new();

    build_cbw(cbw, 0x4, 40, 0x80, 6, cdb); /* transfer_len > send_len (residue) */
    ck_assert_int_eq(disk_provider_handle_out(dp, cbw, sizeof(cbw), queue), 0);
    ck_assert_int_eq(g_queue_get_length(queue), 2);
    g_queue_free_full(queue, free_queue_item);
    queue = g_queue_new();

    /* 3. READ CAPACITY (10) (0x25) */
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x25;
    build_cbw(cbw, 0x5, 8, 0x80, 10, cdb);
    ck_assert_int_eq(disk_provider_handle_out(dp, cbw, sizeof(cbw), queue), 0);
    ck_assert_int_eq(g_queue_get_length(queue), 2);
    g_queue_free_full(queue, free_queue_item);
    queue = g_queue_new();

    /* 4. SERVICE ACTION IN (16) (0x9E) */
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x9E;
    cdb[1] = 0x10; /* READ CAPACITY (16) */
    build_cbw(cbw, 0x6, 32, 0x80, 16, cdb);
    ck_assert_int_eq(disk_provider_handle_out(dp, cbw, sizeof(cbw), queue), 0);
    ck_assert_int_eq(g_queue_get_length(queue), 2);
    g_queue_free_full(queue, free_queue_item);
    queue = g_queue_new();

    build_cbw(cbw, 0x7, 16, 0x80, 16, cdb); /* transfer_len < send_len */
    ck_assert_int_eq(disk_provider_handle_out(dp, cbw, sizeof(cbw), queue), 0);
    ck_assert_int_eq(g_queue_get_length(queue), 2);
    g_queue_free_full(queue, free_queue_item);
    queue = g_queue_new();

    cdb[1] = 0x00; /* Other service action */
    build_cbw(cbw, 0x8, 32, 0x80, 16, cdb);
    ck_assert_int_eq(disk_provider_handle_out(dp, cbw, sizeof(cbw), queue), 0);
    ck_assert_int_eq(g_queue_get_length(queue), 1); /* CSW only, status=1 */
    g_queue_free_full(queue, free_queue_item);
    queue = g_queue_new();

    /* 5. MODE SENSE (6) (0x1A) */
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x1A;
    build_cbw(cbw, 0x9, 4, 0x80, 6, cdb);
    ck_assert_int_eq(disk_provider_handle_out(dp, cbw, sizeof(cbw), queue), 0);
    ck_assert_int_eq(g_queue_get_length(queue), 2);
    g_queue_free_full(queue, free_queue_item);
    queue = g_queue_new();

    build_cbw(cbw, 0x10, 2, 0x80, 6, cdb); /* transfer_len < send_len */
    ck_assert_int_eq(disk_provider_handle_out(dp, cbw, sizeof(cbw), queue), 0);
    ck_assert_int_eq(g_queue_get_length(queue), 2);
    g_queue_free_full(queue, free_queue_item);
    queue = g_queue_new();

    /* 6. MODE SENSE (10) (0x5A) */
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x5A;
    build_cbw(cbw, 0x11, 8, 0x80, 10, cdb);
    ck_assert_int_eq(disk_provider_handle_out(dp, cbw, sizeof(cbw), queue), 0);
    ck_assert_int_eq(g_queue_get_length(queue), 2);
    g_queue_free_full(queue, free_queue_item);
    queue = g_queue_new();

    build_cbw(cbw, 0x12, 4, 0x80, 10, cdb); /* transfer_len < send_len */
    ck_assert_int_eq(disk_provider_handle_out(dp, cbw, sizeof(cbw), queue), 0);
    ck_assert_int_eq(g_queue_get_length(queue), 2);
    g_queue_free_full(queue, free_queue_item);
    queue = g_queue_new();

    /* 7. READ FORMAT CAPACITIES (0x23) */
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x23;
    build_cbw(cbw, 0x13, 12, 0x80, 10, cdb);
    ck_assert_int_eq(disk_provider_handle_out(dp, cbw, sizeof(cbw), queue), 0);
    ck_assert_int_eq(g_queue_get_length(queue), 2);
    g_queue_free_full(queue, free_queue_item);
    queue = g_queue_new();

    build_cbw(cbw, 0x14, 8, 0x80, 10, cdb); /* transfer_len < send_len */
    ck_assert_int_eq(disk_provider_handle_out(dp, cbw, sizeof(cbw), queue), 0);
    ck_assert_int_eq(g_queue_get_length(queue), 2);
    g_queue_free_full(queue, free_queue_item);
    queue = g_queue_new();

    /* 8. READ (10) (0x28) */
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x28;
    put_u32be(cdb + 2, 0); /* LBA = 0 */
    put_u16be(cdb + 7, 1); /* 1 block = 512 bytes */
    build_cbw(cbw, 0x15, 512, 0x80, 10, cdb);
    ck_assert_int_eq(disk_provider_handle_out(dp, cbw, sizeof(cbw), queue), 0);
    ck_assert_int_eq(g_queue_get_length(queue), 2);
    g_queue_free_full(queue, free_queue_item);
    queue = g_queue_new();

    /* READ (10) with 0 blocks */
    put_u16be(cdb + 7, 0);
    build_cbw(cbw, 0x16, 0, 0x80, 10, cdb);
    ck_assert_int_eq(disk_provider_handle_out(dp, cbw, sizeof(cbw), queue), 0);
    ck_assert_int_eq(g_queue_get_length(queue), 1); /* CSW only because req_bytes is 0 */
    g_queue_free_full(queue, free_queue_item);
    queue = g_queue_new();

    /* READ (10) with pread failure */
    put_u16be(cdb + 7, 1);
    build_cbw(cbw, 0x17, 512, 0x80, 10, cdb);
    mock_pread_fail = TRUE;
    ck_assert_int_eq(disk_provider_handle_out(dp, cbw, sizeof(cbw), queue), 0);
    mock_pread_fail = FALSE;
    ck_assert_int_eq(g_queue_get_length(queue), 2);
    g_queue_free_full(queue, free_queue_item);
    queue = g_queue_new();

    /* 9. WRITE (10) (0x2A) */
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x2A;
    put_u32be(cdb + 2, 0); /* LBA = 0 */
    put_u16be(cdb + 7, 0); /* 0 blocks */
    build_cbw(cbw, 0x18, 0, 0, 10, cdb);
    ck_assert_int_eq(disk_provider_handle_out(dp, cbw, sizeof(cbw), queue), 0);
    ck_assert_int_eq(g_queue_get_length(queue), 1); /* CSW */
    g_queue_free_full(queue, free_queue_item);
    queue = g_queue_new();

    /* WRITE (10) with 2 blocks (1024 bytes) */
    put_u16be(cdb + 7, 2);
    build_cbw(cbw, 0x19, 1024, 0, 10, cdb);
    ck_assert_int_eq(disk_provider_handle_out(dp, cbw, sizeof(cbw), queue), 0);
    ck_assert_int_eq(g_queue_get_length(queue), 0); /* No CSW yet */
    ck_assert(dp->has_pending_write);

    /* Test pwrite returning 0 */
    mock_pwrite_zero = TRUE;
    uint8_t write_chunk[512];
    memset(write_chunk, 0x55, sizeof(write_chunk));
    ck_assert_int_eq(disk_provider_handle_out(dp, write_chunk, sizeof(write_chunk), queue), 0);
    mock_pwrite_zero = FALSE;
    ck_assert(dp->has_pending_write);

    /* Send first chunk (512 bytes) */
    ck_assert_int_eq(disk_provider_handle_out(dp, write_chunk, sizeof(write_chunk), queue), 0);
    ck_assert(dp->has_pending_write);
    ck_assert_int_eq(g_queue_get_length(queue), 0);

    /* Send second chunk (512 bytes) -> completes write */
    ck_assert_int_eq(disk_provider_handle_out(dp, write_chunk, sizeof(write_chunk), queue), 0);
    ck_assert(!dp->has_pending_write);
    ck_assert_int_eq(g_queue_get_length(queue), 1); /* CSW returned */
    g_queue_free_full(queue, free_queue_item);
    queue = g_queue_new();

    /* 10. SYNCHRONIZE CACHE (10) (0x35) */
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x35;
    build_cbw(cbw, 0x20, 0, 0, 10, cdb);
    ck_assert_int_eq(disk_provider_handle_out(dp, cbw, sizeof(cbw), queue), 0);
    ck_assert_int_eq(g_queue_get_length(queue), 1);
    g_queue_free_full(queue, free_queue_item);
    queue = g_queue_new();

    /* 11. PREVENT ALLOW MEDIUM REMOVAL (0x1E) */
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x1E;
    build_cbw(cbw, 0x21, 0, 0, 6, cdb);
    ck_assert_int_eq(disk_provider_handle_out(dp, cbw, sizeof(cbw), queue), 0);
    ck_assert_int_eq(g_queue_get_length(queue), 1);
    g_queue_free_full(queue, free_queue_item);
    queue = g_queue_new();

    /* 12. REQUEST SENSE (0x03) */
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x03;
    build_cbw(cbw, 0x22, 18, 0x80, 6, cdb);
    ck_assert_int_eq(disk_provider_handle_out(dp, cbw, sizeof(cbw), queue), 0);
    ck_assert_int_eq(g_queue_get_length(queue), 2);
    g_queue_free_full(queue, free_queue_item);
    queue = g_queue_new();

    build_cbw(cbw, 0x23, 8, 0x80, 6, cdb); /* transfer_len < send_len */
    ck_assert_int_eq(disk_provider_handle_out(dp, cbw, sizeof(cbw), queue), 0);
    ck_assert_int_eq(g_queue_get_length(queue), 2);
    g_queue_free_full(queue, free_queue_item);
    queue = g_queue_new();

    /* 13. Unhandled opcode (0xFF) */
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0xFF;
    build_cbw(cbw, 0x24, 0, 0, 6, cdb);
    ck_assert_int_eq(disk_provider_handle_out(dp, cbw, sizeof(cbw), queue), 0);
    ck_assert_int_eq(g_queue_get_length(queue), 1);
    g_queue_free_full(queue, free_queue_item);
    queue = g_queue_new();

    /* Also test zero total blocks with READ CAPACITY (10) and (16) */
    dp->total_blocks = 0;
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x25;
    build_cbw(cbw, 0x25, 8, 0x80, 10, cdb);
    ck_assert_int_eq(disk_provider_handle_out(dp, cbw, sizeof(cbw), queue), 0);
    g_queue_free_full(queue, free_queue_item);
    queue = g_queue_new();

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x9E;
    cdb[1] = 0x10;
    build_cbw(cbw, 0x26, 32, 0x80, 16, cdb);
    ck_assert_int_eq(disk_provider_handle_out(dp, cbw, sizeof(cbw), queue), 0);
    g_queue_free_full(queue, free_queue_item);
    queue = g_queue_new();

    /* Test SYNCHRONIZE CACHE with dp->fd < 0 */
    int saved_fd = dp->fd;
    dp->fd = -1;
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x35;
    build_cbw(cbw, 0x27, 0, 0, 10, cdb);
    ck_assert_int_eq(disk_provider_handle_out(dp, cbw, sizeof(cbw), queue), 0);
    g_queue_free_full(queue, free_queue_item);
    queue = g_queue_new();
    dp->fd = saved_fd;

    g_queue_free(queue);
    disk_provider_free(dp);
    unlink(tmp_path);
}
END_TEST

START_TEST(test_disk_provider_endpoint_ops)
{
    char tmp_path[] = "/tmp/usbvcr_disk_test_XXXXXX";
    int fd = mkstemp(tmp_path);
    ck_assert_int_ge(fd, 0);
    uint8_t dummy[512] = { 0 };
    ck_assert_int_eq(write(fd, dummy, sizeof(dummy)), sizeof(dummy));
    close(fd);

    disk_provider_t *dp = disk_provider_new(tmp_path, 0, 0);
    ck_assert_ptr_nonnull(dp);

    const flow_endpoint_ops_t *ops = disk_provider_get_ops();
    ck_assert_ptr_nonnull(ops);
    ck_assert(ops->produce_in == NULL);
    ck_assert(ops->tick == NULL);
    ck_assert(ops->consume_out != NULL);
    ck_assert(ops->free != NULL);

    GQueue *queue = g_queue_new();
    uint8_t cbw[31];
    uint8_t cdb[16] = { 0 };
    cdb[0] = 0x00; /* TEST UNIT READY */
    build_cbw(cbw, 0x1, 0, 0, 6, cdb);

    ck_assert_int_eq(ops->consume_out(dp, 0x02, cbw, sizeof(cbw), queue), 0);
    ck_assert_int_eq(g_queue_get_length(queue), 1);
    g_queue_free_full(queue, free_queue_item);

    ops->free(dp);
    unlink(tmp_path);
}
END_TEST

static Suite *disk_provider_suite(void)
{
    Suite *s = suite_create("disk_provider");
    TCase *tc = tcase_create("core");

    tcase_add_test(tc, test_disk_provider_new_and_properties);
    tcase_add_test(tc, test_disk_provider_queue_helpers);
    tcase_add_test(tc, test_disk_provider_handle_out_invalid);
    tcase_add_test(tc, test_disk_provider_scsi_commands);
    tcase_add_test(tc, test_disk_provider_endpoint_ops);

    suite_add_tcase(s, tc);
    return s;
}

int main(void)
{
    SRunner *sr = srunner_create(disk_provider_suite());
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
