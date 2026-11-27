/* Unit tests for src/core/usb_core.c.
 *
 * usb_core_run() drives the whole engine over a socket: it does the hello
 * handshake, announces the device, then serves control / configuration /
 * interrupt traffic by delegating to a usb_device_t backend.
 *
 * We exercise it end-to-end over a socketpair: the engine runs in its own
 * thread on one fd, the test acts as the peer (the usbredir client) on the
 * other, speaking the same wire transport. A small fake backend records the
 * calls the engine makes and serves canned responses.
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <check.h>

#include "usb_core.h"
#include "transport.h"
#include "protocol.h"
#include "device.h"

/* ------------------------------------------------------------------ *
 * Fake device backend                                                 *
 * ------------------------------------------------------------------ */

/* 18-byte device descriptor: vendor 0x1234, product 0x5678, bcdDevice 0x0100,
 * bMaxPacketSize0 = 8. */
static const uint8_t DEV_DESC[18] = {
    0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x08,
    0x34, 0x12, 0x78, 0x56, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x01,
};

/* 18-byte SuperSpeed device descriptor: bcdUSB = 0x0300, bMaxPacketSize0 = 9 (512 bytes) */
static const uint8_t SS_DEV_DESC[18] = {
    0x12, 0x01, 0x00, 0x03, 0x00, 0x00, 0x00, 0x09,
    0x34, 0x12, 0x78, 0x56, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x01,
};

/* 34-byte HID-style config descriptor with one interrupt IN endpoint 0x81.
 * (Same shape as the descriptor exercised in test_descriptor.c.) */
static const uint8_t CFG_DESC[34] = {
    0x09, 0x02, 0x22, 0x00, 0x01, 0x01, 0x00, 0xA0,
    0x32, 0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01,
    0x01, 0x00, 0x09, 0x21, 0x11, 0x01, 0x00, 0x01,
    0x22, 0x3F, 0x00, 0x07, 0x05, 0x81, 0x03, 0x08,
    0x00, 0x0A,
};

typedef struct {
    /* descriptor overrides */
    const uint8_t *custom_dev_desc;
    uint16_t custom_dev_desc_len;
    const uint8_t *custom_cfg_desc;
    uint16_t custom_cfg_desc_len;
    int      fail_get_descriptor;

    /* control() behaviour */
    int      control_ret;               /* value control() returns (-1 default) */
    uint8_t  control_data[64];          /* IN data returned when control_ret==0 */
    uint16_t control_data_len;
    uint8_t *large_control_data;

    /* control() observations */
    int      control_calls;
    usb_control_packet_header_t last_setup;
    int      set_config_value;          /* -1 until a SET_CONFIGURATION arrives */

    /* ep_out() observations */
    uint8_t  ep_out_buf[64];
    size_t   ep_out_len;
    uint8_t  ep_out_ep;

    /* ep_in() canned single report */
    uint8_t  ep_in_ep;
    const uint8_t *ep_in_data;
    int      ep_in_len;
    int      ep_in_sent;

    int      tick_calls;
    int      speed_ret;
} fake_state;

static int fake_get_descriptor(usb_device_t *dev, uint8_t type, uint8_t index,
                               uint16_t lang, const uint8_t **out, uint16_t *len)
{
    fake_state *st = dev->state;
    (void)index; (void)lang;
    if (st->fail_get_descriptor)
        return -1;
    if (type == USB_DESC_DEVICE) {
        if (st->custom_dev_desc) {
            *out = st->custom_dev_desc;
            *len = st->custom_dev_desc_len;
        } else {
            *out = DEV_DESC;
            *len = sizeof(DEV_DESC);
        }
        return 0;
    }
    if (type == USB_DESC_CONFIG) {
        if (st->custom_cfg_desc) {
            *out = st->custom_cfg_desc;
            *len = st->custom_cfg_desc_len;
        } else {
            *out = CFG_DESC;
            *len = sizeof(CFG_DESC);
        }
        return 0;
    }
    return -1;
}

static int fake_control(usb_device_t *dev,
                        const usb_control_packet_header_t *setup,
                        uint8_t *data, uint16_t *len)
{
    fake_state *st = dev->state;
    st->control_calls++;
    st->last_setup = *setup;
    if (setup->request == USB_REQ_SET_CONFIGURATION)
        st->set_config_value = setup->value;

    if (st->control_ret == 0) {
        if (st->large_control_data && st->control_data_len > 0) {
            memcpy(data, st->large_control_data, st->control_data_len);
            *len = st->control_data_len;
        } else {
            memcpy(data, st->control_data, st->control_data_len);
            *len = st->control_data_len;
        }
    }
    return st->control_ret;
}

static int fake_ep_in(usb_device_t *dev, uint8_t ep, uint8_t *buf, size_t cap)
{
    fake_state *st = dev->state;
    if (ep != st->ep_in_ep || st->ep_in_sent || st->ep_in_len <= 0)
        return 0;                        /* NAK: nothing to send */
    int n = st->ep_in_len;
    if ((size_t)n > cap)
        n = (int)cap;
    memcpy(buf, st->ep_in_data, n);
    st->ep_in_sent = 1;
    return n;
}

static int fake_ep_out(usb_device_t *dev, uint8_t ep, const uint8_t *buf, size_t len)
{
    fake_state *st = dev->state;
    st->ep_out_ep = ep;
    st->ep_out_len = len > sizeof(st->ep_out_buf) ? sizeof(st->ep_out_buf) : len;
    memcpy(st->ep_out_buf, buf, st->ep_out_len);
    return 0;
}

static void fake_tick(usb_device_t *dev)
{
    ((fake_state *)dev->state)->tick_calls++;
}

static int fake_get_speed(usb_device_t *dev)
{
    return ((fake_state *)dev->state)->speed_ret;
}

static const usb_device_ops_t FAKE_OPS = {
    .name           = "fake",
    .get_descriptor = fake_get_descriptor,
    .control        = fake_control,
    .ep_in          = fake_ep_in,
    .ep_out         = fake_ep_out,
    .tick           = fake_tick,
    .get_speed      = fake_get_speed,
};

/* ------------------------------------------------------------------ *
 * Harness: run the engine on a thread, talk to it as the peer         *
 * ------------------------------------------------------------------ */

typedef struct {
    int fds[2];             /* fds[0] = peer (test), fds[1] = engine */
    fake_state state;
    usb_device_t dev;
    usb_record_ctx_t ctx;
    pthread_t thread;
} harness;

static void *engine_thread(void *arg)
{
    harness *h = arg;
    usb_core_run(h->fds[1], &h->dev);
    return NULL;
}

static void harness_start(harness *h)
{
    memset(&h->state, 0, sizeof(h->state));
    h->state.control_ret = -1;           /* stall unless a test opts in */
    h->state.set_config_value = -1;
    h->state.speed_ret = -1;
    h->dev.ops = &FAKE_OPS;
    h->dev.state = &h->state;
    h->ctx.bus = 0;
    h->ctx.device = 1;

    ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, h->fds), 0);

    /* Give the engine fd a short recv timeout so its idle branch (tick +
     * interrupt draining) is reachable; a bare socketpair never times out. */
    struct timeval tv = {0, 100 * 1000};   /* 100 ms */
    setsockopt(h->fds[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);
    ck_assert_int_eq(pthread_create(&h->thread, &attr, engine_thread, h), 0);
    pthread_attr_destroy(&attr);
}

/* Close the peer end (engine sees EOF and returns) and join. All bytes the
 * peer already sent are delivered before EOF, so the backend state reflects
 * every request once join() returns. */
static void harness_stop(harness *h)
{
    close(h->fds[0]);
    pthread_join(h->thread, NULL);
    close(h->fds[1]);
}

/* Peer-side frame recv: skip idle results (the peer fd has no timeout, so
 * these should not occur, but stay defensive). */
static int peer_recv(harness *h, usb_header_t *hdr, uint8_t *buf, size_t *len)
{
    int r;
    while ((r = usb_conn_recv(h->fds[0], hdr, buf, USB_VCR_BUF_SIZE, len, &h->ctx)) == 1)
        ;
    return r;
}

/* Do the hello handshake and consume the three announcement frames. When
 * `connect_out` is non-NULL, the RT_DEVICE_CONNECT payload is copied out. */
static void peer_handshake(harness *h, usb_device_connect_header_t *connect_out)
{
    usb_header_t hdr;
    uint8_t buf[USB_VCR_BUF_SIZE];
    size_t len;

    /* Advertise no capabilities: the negotiated set is empty, so the header
     * stays 12 bytes (no 64-bit ids) for the whole session. */
    usb_hello_header_t hello;
    memset(&hello, 0, sizeof(hello));
    strncpy(hello.version, "test-peer", sizeof(hello.version) - 1);
    ck_assert_int_eq(usb_conn_send(h->fds[0], RT_HELLO, 0, &hello, sizeof(hello), &h->ctx), 0);

    ck_assert_int_eq(peer_recv(h, &hdr, buf, &len), 0);
    ck_assert_uint_eq(hdr.type, RT_HELLO);

    ck_assert_int_eq(peer_recv(h, &hdr, buf, &len), 0);
    ck_assert_uint_eq(hdr.type, RT_INTERFACE_INFO);

    ck_assert_int_eq(peer_recv(h, &hdr, buf, &len), 0);
    ck_assert_uint_eq(hdr.type, RT_EP_INFO);

    ck_assert_int_eq(peer_recv(h, &hdr, buf, &len), 0);
    ck_assert_uint_eq(hdr.type, RT_DEVICE_CONNECT);
    if (connect_out)
        memcpy(connect_out, buf, sizeof(*connect_out));
}

static void peer_handshake_caps(harness *h, uint32_t caps, usb_device_connect_header_t *connect_out)
{
    usb_header_t hdr;
    uint8_t buf[USB_VCR_BUF_SIZE];
    size_t len;

    usb_hello_header_t hello;
    memset(&hello, 0, sizeof(hello));
    strncpy(hello.version, "test-peer", sizeof(hello.version) - 1);
    hello.capabilities[0] = caps;
    ck_assert_int_eq(usb_conn_send(h->fds[0], RT_HELLO, 0, &hello, sizeof(hello), &h->ctx), 0);

    ck_assert_int_eq(peer_recv(h, &hdr, buf, &len), 0);
    ck_assert_uint_eq(hdr.type, RT_HELLO);

    ck_assert_int_eq(peer_recv(h, &hdr, buf, &len), 0);
    ck_assert_uint_eq(hdr.type, RT_INTERFACE_INFO);

    ck_assert_int_eq(peer_recv(h, &hdr, buf, &len), 0);
    ck_assert_uint_eq(hdr.type, RT_EP_INFO);

    ck_assert_int_eq(peer_recv(h, &hdr, buf, &len), 0);
    ck_assert_uint_eq(hdr.type, RT_DEVICE_CONNECT);
    if (connect_out)
        memcpy(connect_out, buf, sizeof(*connect_out));
}

/* Send a control setup (+ optional OUT data) as one RT_CONTROL_PACKET. */
static void peer_send_control(harness *h, int64_t id,
                              const usb_control_packet_header_t *setup)
{
    ck_assert_int_eq(usb_conn_send(h->fds[0], RT_CONTROL_PACKET, id,
                                   setup, sizeof(*setup), &h->ctx), 0);
}

/* ------------------------------------------------------------------ *
 * Tests                                                               *
 * ------------------------------------------------------------------ */

/* Handshake replies with hello + the three announcement frames, and the
 * device_connect fields are derived from the device descriptor. */
START_TEST(test_handshake_and_announce)
{
    harness h;
    harness_start(&h);

    usb_device_connect_header_t c;
    peer_handshake(&h, &c);

    ck_assert_uint_eq(c.vendor_id, 0x1234);
    ck_assert_uint_eq(c.product_id, 0x5678);
    ck_assert_uint_eq(c.speed, USB_VCR_SPEED_FULL);

    harness_stop(&h);
}
END_TEST

START_TEST(test_announce_speed_override)
{
    harness h;
    memset(&h.state, 0, sizeof(h.state));
    h.state.control_ret = -1;
    h.state.set_config_value = -1;
    h.state.speed_ret = USB_VCR_SPEED_HIGH;
    h.dev.ops = &FAKE_OPS;
    h.dev.state = &h.state;
    h.ctx.bus = 0;
    h.ctx.device = 1;

    ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, h.fds), 0);
    struct timeval tv = {0, 100 * 1000};
    setsockopt(h.fds[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ck_assert_int_eq(pthread_create(&h.thread, NULL, engine_thread, &h), 0);

    usb_device_connect_header_t c;
    peer_handshake(&h, &c);

    ck_assert_uint_eq(c.speed, USB_VCR_SPEED_HIGH);

    harness_stop(&h);
}
END_TEST

/* With no backend rule for GET_DESCRIPTOR, the engine serves it straight from
 * the descriptor table and truncates the reply to wLength. */
START_TEST(test_get_descriptor_fallback)
{
    harness h;
    harness_start(&h);
    peer_handshake(&h, NULL);

    usb_control_packet_header_t setup;
    memset(&setup, 0, sizeof(setup));
    setup.requesttype = USB_DIR_IN;
    setup.request     = USB_REQ_GET_DESCRIPTOR;
    setup.value       = (USB_DESC_DEVICE << 8);
    setup.length      = 8;                 /* ask for fewer than 18 bytes */
    peer_send_control(&h, 42, &setup);

    usb_header_t hdr;
    uint8_t buf[USB_VCR_BUF_SIZE];
    size_t len;
    ck_assert_int_eq(peer_recv(&h, &hdr, buf, &len), 0);
    ck_assert_uint_eq(hdr.type, RT_CONTROL_PACKET);
    ck_assert_int_eq(hdr.id, 42);

    usb_control_packet_header_t reply;
    memcpy(&reply, buf, sizeof(reply));
    ck_assert_uint_eq(reply.status, 0);    /* ACK */
    ck_assert_uint_eq(reply.length, 8);    /* truncated to wLength */
    ck_assert_mem_eq(buf + sizeof(reply), DEV_DESC, 8);

    harness_stop(&h);
}
END_TEST

/* A backend that handles the setup (returns 0) short-circuits the fallback:
 * its data is returned verbatim with an ACK. */
START_TEST(test_control_backend_handled)
{
    harness h;
    harness_start(&h);
    h.state.control_ret = 0;
    memcpy(h.state.control_data, "ABCD", 4);
    h.state.control_data_len = 4;
    peer_handshake(&h, NULL);

    usb_control_packet_header_t setup;
    memset(&setup, 0, sizeof(setup));
    setup.requesttype = USB_DIR_IN | USB_TYPE_CLASS;
    setup.request     = 0x01;              /* class-specific */
    setup.length      = 4;
    peer_send_control(&h, 7, &setup);

    usb_header_t hdr;
    uint8_t buf[USB_VCR_BUF_SIZE];
    size_t len;
    ck_assert_int_eq(peer_recv(&h, &hdr, buf, &len), 0);
    ck_assert_uint_eq(hdr.type, RT_CONTROL_PACKET);

    usb_control_packet_header_t reply;
    memcpy(&reply, buf, sizeof(reply));
    ck_assert_uint_eq(reply.status, 0);
    ck_assert_uint_eq(reply.length, 4);
    ck_assert_mem_eq(buf + sizeof(reply), "ABCD", 4);

    harness_stop(&h);
}
END_TEST

/* An unhandled request that isn't a standard GET_DESCRIPTOR stalls. */
START_TEST(test_control_stall)
{
    harness h;
    harness_start(&h);                     /* control_ret stays -1 */
    peer_handshake(&h, NULL);

    usb_control_packet_header_t setup;
    memset(&setup, 0, sizeof(setup));
    setup.requesttype = USB_TYPE_CLASS;    /* OUT class request, no fallback */
    setup.request     = 0x09;
    peer_send_control(&h, 5, &setup);

    usb_header_t hdr;
    uint8_t buf[USB_VCR_BUF_SIZE];
    size_t len;
    ck_assert_int_eq(peer_recv(&h, &hdr, buf, &len), 0);
    ck_assert_uint_eq(hdr.type, RT_CONTROL_PACKET);

    usb_control_packet_header_t reply;
    memcpy(&reply, buf, sizeof(reply));
    ck_assert_uint_eq(reply.status, 1);    /* stall */
    ck_assert_uint_eq(reply.length, 0);

    harness_stop(&h);
}
END_TEST

/* A control packet shorter than the setup header is dropped without a reply.
 * Prove it by following with a valid request and getting exactly that reply. */
START_TEST(test_control_too_short)
{
    harness h;
    harness_start(&h);
    peer_handshake(&h, NULL);

    uint8_t stub[3] = { 1, 2, 3 };
    ck_assert_int_eq(usb_conn_send(h.fds[0], RT_CONTROL_PACKET, 1, stub, sizeof(stub), &h.ctx), 0);

    usb_control_packet_header_t setup;
    memset(&setup, 0, sizeof(setup));
    setup.requesttype = USB_DIR_IN;
    setup.request     = USB_REQ_GET_DESCRIPTOR;
    setup.value       = (USB_DESC_DEVICE << 8);
    setup.length      = 8;
    peer_send_control(&h, 99, &setup);

    usb_header_t hdr;
    uint8_t buf[USB_VCR_BUF_SIZE];
    size_t len;
    ck_assert_int_eq(peer_recv(&h, &hdr, buf, &len), 0);
    ck_assert_uint_eq(hdr.type, RT_CONTROL_PACKET);
    ck_assert_int_eq(hdr.id, 99);          /* the short one produced nothing */

    harness_stop(&h);
}
END_TEST

/* SET_CONFIGURATION arrives as its own message: the engine synthesizes a setup
 * to drive the backend, then acks with RT_CONFIGURATION_STATUS {0, cfg}. */
START_TEST(test_set_configuration)
{
    harness h;
    harness_start(&h);
    peer_handshake(&h, NULL);

    uint8_t cfg = 1;
    ck_assert_int_eq(usb_conn_send(h.fds[0], RT_SET_CONFIGURATION, 3, &cfg, 1, &h.ctx), 0);

    usb_header_t hdr;
    uint8_t buf[USB_VCR_BUF_SIZE];
    size_t len;
    ck_assert_int_eq(peer_recv(&h, &hdr, buf, &len), 0);
    ck_assert_uint_eq(hdr.type, RT_CONFIGURATION_STATUS);
    ck_assert_uint_eq(len, 2);
    ck_assert_uint_eq(buf[0], 0);          /* success */
    ck_assert_uint_eq(buf[1], 1);          /* echoed config value */

    harness_stop(&h);

    ck_assert_int_eq(h.state.set_config_value, 1);
    ck_assert_uint_eq(h.state.last_setup.request, USB_REQ_SET_CONFIGURATION);
}
END_TEST

/* START_INTERRUPT_RECEIVING drains the backend's ep_in and forwards a report
 * as an RT_INTERRUPT_PACKET. */
START_TEST(test_interrupt_in)
{
    static const uint8_t report[5] = { 'H', 'E', 'L', 'L', 'O' };
    harness h;
    harness_start(&h);
    h.state.ep_in_ep = 0x81;
    h.state.ep_in_data = report;
    h.state.ep_in_len = sizeof(report);
    peer_handshake(&h, NULL);

    uint8_t ep = 0x81;
    ck_assert_int_eq(usb_conn_send(h.fds[0], RT_START_INTERRUPT_RECEIVING, 0, &ep, 1, &h.ctx), 0);

    usb_header_t hdr;
    uint8_t buf[USB_VCR_BUF_SIZE];
    size_t len;
    ck_assert_int_eq(peer_recv(&h, &hdr, buf, &len), 0);
    ck_assert_uint_eq(hdr.type, RT_INTERRUPT_PACKET);

    usb_interrupt_packet_header_t ih;
    memcpy(&ih, buf, sizeof(ih));
    ck_assert_uint_eq(ih.endpoint, 0x81);
    ck_assert_uint_eq(ih.length, sizeof(report));
    ck_assert_mem_eq(buf + sizeof(ih), report, sizeof(report));

    harness_stop(&h);
}
END_TEST

/* An inbound RT_INTERRUPT_PACKET (OUT) is routed to the backend's ep_out. */
START_TEST(test_interrupt_out)
{
    harness h;
    harness_start(&h);
    peer_handshake(&h, NULL);

    uint8_t frame[sizeof(usb_interrupt_packet_header_t) + 3];
    usb_interrupt_packet_header_t ih = { 0x01, 0, 3 };
    memcpy(frame, &ih, sizeof(ih));
    memcpy(frame + sizeof(ih), "XYZ", 3);
    ck_assert_int_eq(usb_conn_send(h.fds[0], RT_INTERRUPT_PACKET, 0, frame, sizeof(frame), &h.ctx), 0);

    harness_stop(&h);                      /* join flushes the OUT into ep_out */

    ck_assert_uint_eq(h.state.ep_out_ep, 0x01);
    ck_assert_uint_eq(h.state.ep_out_len, 3);
    ck_assert_mem_eq(h.state.ep_out_buf, "XYZ", 3);
}
END_TEST

/* When the peer is idle, the recv timeout turns the wait into a periodic tick
 * so streaming backends keep making progress. */
START_TEST(test_idle_tick)
{
    harness h;
    harness_start(&h);
    peer_handshake(&h, NULL);

    usleep(300 * 1000);                    /* > a couple of 100 ms idle ticks */

    harness_stop(&h);

    ck_assert_int_gt(h.state.tick_calls, 0);
}
END_TEST

START_TEST(test_announce_superspeed_and_caps)
{
    harness h;
    memset(&h.state, 0, sizeof(h.state));
    h.state.control_ret = -1;
    h.state.set_config_value = -1;
    h.state.speed_ret = -1;
    h.state.custom_dev_desc = SS_DEV_DESC;
    h.state.custom_dev_desc_len = sizeof(SS_DEV_DESC);
    h.dev.ops = &FAKE_OPS;
    h.dev.state = &h.state;
    h.ctx.bus = 0;
    h.ctx.device = 1;

    ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, h.fds), 0);
    struct timeval tv = {0, 100 * 1000};
    setsockopt(h.fds[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ck_assert_int_eq(pthread_create(&h.thread, NULL, engine_thread, &h), 0);

    uint32_t caps = (1u << USB_VCR_CAP_CONNECT_DEVICE_VERSION) |
                    (1u << USB_VCR_CAP_EP_INFO_MAX_PACKET_SIZE) |
                    (1u << USB_VCR_CAP_BULK_STREAMS);
    usb_device_connect_header_t c;
    peer_handshake_caps(&h, caps, &c);

    ck_assert_uint_eq(c.speed, USB_VCR_SPEED_SUPER);

    harness_stop(&h);
}
END_TEST

START_TEST(test_handshake_bad_packet)
{
    harness h;
    harness_start(&h);

    /* Send RT_RESET instead of RT_HELLO */
    ck_assert_int_eq(usb_conn_send(h.fds[0], RT_RESET, 0, NULL, 0, &h.ctx), 0);

    /* Engine sees wrong packet type, logs warning and terminates */
    pthread_join(h.thread, NULL);
    close(h.fds[0]);
    close(h.fds[1]);
}
END_TEST

START_TEST(test_announce_missing_descriptors)
{
    harness h;
    memset(&h.state, 0, sizeof(h.state));
    h.state.fail_get_descriptor = 1;
    h.dev.ops = &FAKE_OPS;
    h.dev.state = &h.state;
    h.ctx.bus = 0;
    h.ctx.device = 1;
    ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, h.fds), 0);
    ck_assert_int_eq(pthread_create(&h.thread, NULL, engine_thread, &h), 0);

    usb_hello_header_t hello;
    memset(&hello, 0, sizeof(hello));
    ck_assert_int_eq(usb_conn_send(h.fds[0], RT_HELLO, 0, &hello, sizeof(hello), &h.ctx), 0);

    usb_header_t hdr;
    uint8_t buf[USB_VCR_BUF_SIZE];
    size_t len;
    ck_assert_int_eq(peer_recv(&h, &hdr, buf, &len), 0);
    ck_assert_uint_eq(hdr.type, RT_HELLO);

    pthread_join(h.thread, NULL);
    close(h.fds[0]);
    close(h.fds[1]);
}
END_TEST

START_TEST(test_announce_bad_config)
{
    static const uint8_t bad_cfg[2] = { 0x00, 0x02 }; /* blen == 0 triggers desc_parse_config error */
    harness h;
    memset(&h.state, 0, sizeof(h.state));
    h.state.custom_cfg_desc = bad_cfg;
    h.state.custom_cfg_desc_len = sizeof(bad_cfg);
    h.dev.ops = &FAKE_OPS;
    h.dev.state = &h.state;
    h.ctx.bus = 0;
    h.ctx.device = 1;
    ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, h.fds), 0);
    ck_assert_int_eq(pthread_create(&h.thread, NULL, engine_thread, &h), 0);

    usb_hello_header_t hello;
    memset(&hello, 0, sizeof(hello));
    ck_assert_int_eq(usb_conn_send(h.fds[0], RT_HELLO, 0, &hello, sizeof(hello), &h.ctx), 0);

    usb_header_t hdr;
    uint8_t buf[USB_VCR_BUF_SIZE];
    size_t len;
    ck_assert_int_eq(peer_recv(&h, &hdr, buf, &len), 0);
    ck_assert_uint_eq(hdr.type, RT_HELLO);

    /* Engine fails at announce_device and terminates */
    pthread_join(h.thread, NULL);
    close(h.fds[0]);
    close(h.fds[1]);
}
END_TEST

START_TEST(test_control_large_payload)
{
    harness h;
    harness_start(&h);
    uint8_t large_buf[4090];
    memset(large_buf, 0x55, sizeof(large_buf));
    h.state.control_ret = 0;
    h.state.large_control_data = large_buf;
    h.state.control_data_len = sizeof(large_buf);
    peer_handshake(&h, NULL);

    usb_control_packet_header_t setup;
    memset(&setup, 0, sizeof(setup));
    setup.requesttype = USB_DIR_IN | USB_TYPE_CLASS;
    setup.request     = 0x99;
    setup.length      = sizeof(large_buf);
    peer_send_control(&h, 88, &setup);

    usb_header_t hdr;
    uint8_t *resp = malloc(USB_VCR_BUF_SIZE);
    size_t len;
    ck_assert_int_eq(peer_recv(&h, &hdr, resp, &len), 0);
    ck_assert_uint_eq(hdr.type, RT_CONTROL_PACKET);

    usb_control_packet_header_t reply;
    memcpy(&reply, resp, sizeof(reply));
    ck_assert_uint_eq(reply.status, 0);
    ck_assert_uint_eq(reply.length, sizeof(large_buf));
    ck_assert_mem_eq(resp + sizeof(reply), large_buf, sizeof(large_buf));

    free(resp);
    harness_stop(&h);
}
END_TEST

START_TEST(test_control_get_descriptor_stall)
{
    harness h;
    harness_start(&h);
    peer_handshake(&h, NULL);

    usb_control_packet_header_t setup;
    memset(&setup, 0, sizeof(setup));
    setup.requesttype = USB_DIR_IN;
    setup.request     = USB_REQ_GET_DESCRIPTOR;
    setup.value       = (0x99 << 8); /* Unknown descriptor type */
    setup.length      = 8;
    peer_send_control(&h, 45, &setup);

    usb_header_t hdr;
    uint8_t buf[USB_VCR_BUF_SIZE];
    size_t len;
    ck_assert_int_eq(peer_recv(&h, &hdr, buf, &len), 0);
    ck_assert_uint_eq(hdr.type, RT_CONTROL_PACKET);

    usb_control_packet_header_t reply;
    memcpy(&reply, buf, sizeof(reply));
    ck_assert_uint_eq(reply.status, 1); /* STALL */
    ck_assert_uint_eq(reply.length, 0);

    harness_stop(&h);
}
END_TEST

START_TEST(test_control_get_status)
{
    harness h;
    harness_start(&h);
    peer_handshake(&h, NULL);

    usb_control_packet_header_t setup;
    memset(&setup, 0, sizeof(setup));
    setup.requesttype = USB_DIR_IN | 0x00; /* Device recipient */
    setup.request     = USB_REQ_GET_STATUS;
    setup.length      = 2;
    peer_send_control(&h, 46, &setup);

    usb_header_t hdr;
    uint8_t buf[USB_VCR_BUF_SIZE];
    size_t len;
    ck_assert_int_eq(peer_recv(&h, &hdr, buf, &len), 0);
    ck_assert_uint_eq(hdr.type, RT_CONTROL_PACKET);

    usb_control_packet_header_t reply;
    memcpy(&reply, buf, sizeof(reply));
    ck_assert_uint_eq(reply.status, 0);
    ck_assert_uint_eq(reply.length, 2);
    ck_assert_uint_eq(buf[sizeof(reply)], 0x01); /* Self-powered */
    ck_assert_uint_eq(buf[sizeof(reply) + 1], 0x00);

    harness_stop(&h);
}
END_TEST

START_TEST(test_set_alt_setting)
{
    harness h;
    harness_start(&h);
    peer_handshake(&h, NULL);

    usb_set_alt_setting_header_t req = {
        .interface = 1,
        .alt = 2,
    };
    ck_assert_int_eq(usb_conn_send(h.fds[0], RT_SET_ALT_SETTING, 13, &req, sizeof(req), &h.ctx), 0);

    usb_header_t hdr;
    uint8_t buf[USB_VCR_BUF_SIZE];
    size_t len;
    ck_assert_int_eq(peer_recv(&h, &hdr, buf, &len), 0);
    ck_assert_uint_eq(hdr.type, RT_ALT_SETTING_STATUS);

    usb_alt_setting_status_header_t reply;
    memcpy(&reply, buf, sizeof(reply));
    ck_assert_uint_eq(reply.status, 0);
    ck_assert_uint_eq(reply.interface, 1);
    ck_assert_uint_eq(reply.alt, 2);

    harness_stop(&h);
}
END_TEST

START_TEST(test_iso_stream)
{
    static const uint8_t isoc_data[16] = "ISO_STREAM_TEST";
    harness h;
    harness_start(&h);
    h.state.ep_in_ep = 0x82;
    h.state.ep_in_data = isoc_data;
    h.state.ep_in_len = sizeof(isoc_data);
    peer_handshake(&h, NULL);

    /* Start ISO stream on EP 0x82 */
    uint8_t ep = 0x82;
    ck_assert_int_eq(usb_conn_send(h.fds[0], RT_START_ISO_STREAM, 20, &ep, 1, &h.ctx), 0);

    usb_header_t hdr;
    uint8_t buf[USB_VCR_BUF_SIZE];
    size_t len;
    ck_assert_int_eq(peer_recv(&h, &hdr, buf, &len), 0);
    ck_assert_uint_eq(hdr.type, RT_ISO_STREAM_STATUS);
    usb_iso_stream_status_header_t isostatus;
    memcpy(&isostatus, buf, sizeof(isostatus));
    ck_assert_uint_eq(isostatus.status, 0);
    ck_assert_uint_eq(isostatus.endpoint, 0x82);

    /* Wait for idle tick to poll ISO endpoints and push packet */
    ck_assert_int_eq(peer_recv(&h, &hdr, buf, &len), 0);
    ck_assert_uint_eq(hdr.type, RT_ISO_PACKET);
    usb_iso_packet_header_t isohdr;
    memcpy(&isohdr, buf, sizeof(isohdr));
    ck_assert_uint_eq(isohdr.endpoint, 0x82);
    ck_assert_uint_eq(isohdr.length, sizeof(isoc_data));
    ck_assert_mem_eq(buf + sizeof(isohdr), isoc_data, sizeof(isoc_data));

    /* Stop ISO stream */
    ck_assert_int_eq(usb_conn_send(h.fds[0], RT_STOP_ISO_STREAM, 21, &ep, 1, &h.ctx), 0);
    ck_assert_int_eq(peer_recv(&h, &hdr, buf, &len), 0);
    ck_assert_uint_eq(hdr.type, RT_ISO_STREAM_STATUS);

    harness_stop(&h);
}
END_TEST

START_TEST(test_bulk_in_and_out)
{
    static const uint8_t bulk_data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    harness h;
    harness_start(&h);
    h.state.ep_in_ep = 0x82;
    h.state.ep_in_data = bulk_data;
    h.state.ep_in_len = sizeof(bulk_data);
    peer_handshake(&h, NULL);

    /* 1. Test Bulk IN: Host requests 8 bytes from EP 0x82 */
    usb_bulk_packet_header_t in_req = {
        .endpoint = 0x82,
        .status = 0,
        .length = 8,
        .length_high = 0,
    };
    ck_assert_int_eq(usb_conn_send(h.fds[0], RT_BULK_PACKET, 30, &in_req, sizeof(in_req), &h.ctx), 0);

    usb_header_t hdr;
    uint8_t buf[USB_VCR_BUF_SIZE];
    size_t len;
    ck_assert_int_eq(peer_recv(&h, &hdr, buf, &len), 0);
    ck_assert_uint_eq(hdr.type, RT_BULK_PACKET);
    ck_assert_int_eq(hdr.id, 30);

    usb_bulk_packet_header_t in_resp;
    memcpy(&in_resp, buf, sizeof(in_resp));
    ck_assert_uint_eq(in_resp.endpoint, 0x82);
    ck_assert_uint_eq(in_resp.length, 8);
    ck_assert_mem_eq(buf + sizeof(in_resp), bulk_data, 8);

    /* 1b. Test Bulk IN clamp when requested length exceeds max_in */
    h.state.ep_in_sent = 0;
    usb_bulk_packet_header_t large_in_req = {
        .endpoint = 0x82,
        .status = 0,
        .length = 0xFFFF,
        .length_high = 0x0002,
    };
    ck_assert_int_eq(usb_conn_send(h.fds[0], RT_BULK_PACKET, 35, &large_in_req, sizeof(large_in_req), &h.ctx), 0);
    ck_assert_int_eq(peer_recv(&h, &hdr, buf, &len), 0);
    ck_assert_uint_eq(hdr.type, RT_BULK_PACKET);

    /* 1c. Test Bulk packet too short (dropped) */
    uint8_t short_bulk[3] = { 0x01, 0x02, 0x03 };
    ck_assert_int_eq(usb_conn_send(h.fds[0], RT_BULK_PACKET, 36, short_bulk, sizeof(short_bulk), &h.ctx), 0);

    /* 2. Test Bulk OUT: Host sends 5 bytes to EP 0x02 */
    uint8_t out_frame[sizeof(usb_bulk_packet_header_t) + 5];
    usb_bulk_packet_header_t out_req = {
        .endpoint = 0x02,
        .status = 0,
        .length = 5,
        .length_high = 0,
    };
    memcpy(out_frame, &out_req, sizeof(out_req));
    memcpy(out_frame + sizeof(out_req), "HELLO", 5);
    ck_assert_int_eq(usb_conn_send(h.fds[0], RT_BULK_PACKET, 31, out_frame, sizeof(out_frame), &h.ctx), 0);

    ck_assert_int_eq(peer_recv(&h, &hdr, buf, &len), 0);
    ck_assert_uint_eq(hdr.type, RT_BULK_PACKET);
    ck_assert_int_eq(hdr.id, 31);

    harness_stop(&h);

    ck_assert_uint_eq(h.state.ep_out_ep, 0x02);
    ck_assert_uint_eq(h.state.ep_out_len, 5);
    ck_assert_mem_eq(h.state.ep_out_buf, "HELLO", 5);
}
END_TEST

START_TEST(test_bus_reset_cancel_and_unhandled)
{
    harness h;
    harness_start(&h);
    peer_handshake(&h, NULL);

    /* Stop interrupt receiving */
    uint8_t ep = 0x81;
    ck_assert_int_eq(usb_conn_send(h.fds[0], RT_STOP_INTERRUPT_RECEIVING, 0, &ep, 1, &h.ctx), 0);

    /* Bus reset */
    ck_assert_int_eq(usb_conn_send(h.fds[0], RT_RESET, 0, NULL, 0, &h.ctx), 0);

    /* Cancel data packet */
    ck_assert_int_eq(usb_conn_send(h.fds[0], RT_CANCEL_DATA_PACKET, 999, NULL, 0, &h.ctx), 0);

    /* Unhandled packet type */
    ck_assert_int_eq(usb_conn_send(h.fds[0], 0xFE, 1000, NULL, 0, &h.ctx), 0);

    harness_stop(&h);
}
END_TEST

/* ------------------------------------------------------------------ */

static Suite *usb_core_suite(void)
{
    Suite *s = suite_create("usb_core");

    TCase *tc_hs = tcase_create("handshake");
    tcase_add_test(tc_hs, test_handshake_and_announce);
    tcase_add_test(tc_hs, test_announce_speed_override);
    tcase_add_test(tc_hs, test_announce_superspeed_and_caps);
    tcase_add_test(tc_hs, test_handshake_bad_packet);
    tcase_add_test(tc_hs, test_announce_missing_descriptors);
    tcase_add_test(tc_hs, test_announce_bad_config);
    suite_add_tcase(s, tc_hs);

    TCase *tc_ctl = tcase_create("control");
    tcase_add_test(tc_ctl, test_get_descriptor_fallback);
    tcase_add_test(tc_ctl, test_control_backend_handled);
    tcase_add_test(tc_ctl, test_control_stall);
    tcase_add_test(tc_ctl, test_control_too_short);
    tcase_add_test(tc_ctl, test_control_large_payload);
    tcase_add_test(tc_ctl, test_control_get_descriptor_stall);
    tcase_add_test(tc_ctl, test_control_get_status);
    suite_add_tcase(s, tc_ctl);

    TCase *tc_cfg = tcase_create("config");
    tcase_add_test(tc_cfg, test_set_configuration);
    tcase_add_test(tc_cfg, test_set_alt_setting);
    suite_add_tcase(s, tc_cfg);

    TCase *tc_ep = tcase_create("endpoints");
    tcase_add_test(tc_ep, test_interrupt_in);
    tcase_add_test(tc_ep, test_interrupt_out);
    tcase_add_test(tc_ep, test_idle_tick);
    tcase_add_test(tc_ep, test_iso_stream);
    tcase_add_test(tc_ep, test_bulk_in_and_out);
    tcase_add_test(tc_ep, test_bus_reset_cancel_and_unhandled);
    suite_add_tcase(s, tc_ep);

    return s;
}

int main(void)
{
    SRunner *sr = srunner_create(usb_core_suite());
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
