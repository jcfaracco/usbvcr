/*
 * pcap_backend.c – usb_device_ops backend backed by a usbmon capture.
 *
 * Replays packets recorded in a usbmon pcap or pcapng capture:
 *   get_descriptor -> replays captured GET_DESCRIPTOR replies
 *   control        -> matches SETUP packets and replays captured replies
 *   ep_in          -> yields next captured IN payload for that endpoint
 *   ep_out         -> validates received OUT payload against captured data
 *   tick           -> periodic hook for progress
 */

#include "pcap_backend.h"
#include "pcap/pcap.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <glib.h>

typedef struct {
    uint8_t setup[8];
    GByteArray *reply_data;
} pcap_control_txn_t;

typedef struct {
    pcap_trace_t *trace;
    GHashTable   *descriptors;   /* guint ((type << 8) | index) -> GByteArray* */
    GQueue       *control_queue; /* pcap_control_txn_t* */
    GHashTable   *ep_in_queues;  /* guint (ep) -> GQueue* of GByteArray* */
    GHashTable   *ep_out_queues; /* guint (ep) -> GQueue* of GByteArray* */
} pcap_backend_state_t;

static pcap_control_txn_t *pcap_control_txn_new(const uint8_t setup[8],
                                                const uint8_t *data,
                                                size_t len)
{
    pcap_control_txn_t *txn;

    txn = g_new0(pcap_control_txn_t, 1);
    memcpy(txn->setup, setup, sizeof(txn->setup));
    if (data != NULL && len > 0)
    {
        txn->reply_data = g_byte_array_sized_new((guint)len);
        g_byte_array_append(txn->reply_data, data, (guint)len);
    }
    return txn;
}

static void pcap_control_txn_free(gpointer data)
{
    pcap_control_txn_t *txn;

    txn = (pcap_control_txn_t *)data;
    if (txn == NULL)
    {
        return;
    }
    if (txn->reply_data != NULL)
    {
        g_byte_array_unref(txn->reply_data);
    }
    g_free(txn);
}

static void ep_queue_free(gpointer data)
{
    GQueue *q;

    q = (GQueue *)data;
    if (q != NULL)
    {
        g_queue_free_full(q, (GDestroyNotify)g_byte_array_unref);
    }
}

static void push_ep_payload(GHashTable *ep_table,
                            uint8_t ep,
                            const uint8_t *data,
                            size_t len)
{
    GQueue *q;
    GByteArray *arr;

    q = g_hash_table_lookup(ep_table, GUINT_TO_POINTER((guint)ep));
    if (q == NULL)
    {
        q = g_queue_new();
        g_hash_table_insert(ep_table, GUINT_TO_POINTER((guint)ep), q);
    }

    arr = g_byte_array_sized_new((guint)len);
    if (data != NULL && len > 0)
    {
        g_byte_array_append(arr, data, (guint)len);
    }
    g_queue_push_tail(q, arr);
}

static void record_descriptor(GHashTable *descriptors,
                              uint8_t dtype,
                              uint8_t dindex,
                              const uint8_t *data,
                              size_t len)
{
    guint key;
    GByteArray *buf;

    key = ((guint)dtype << 8) | dindex;
    buf = g_hash_table_lookup(descriptors, GUINT_TO_POINTER(key));
    if (buf == NULL)
    {
        buf = g_byte_array_sized_new((guint)len);
        if (data != NULL && len > 0)
        {
            g_byte_array_append(buf, data, (guint)len);
        }
        g_hash_table_insert(descriptors, GUINT_TO_POINTER(key), buf);
    }
    else if (len > buf->len)
    {
        g_byte_array_set_size(buf, 0);
        if (data != NULL && len > 0)
        {
            g_byte_array_append(buf, data, (guint)len);
        }
    }
}

static void index_control_pair(pcap_backend_state_t *st,
                               const pcap_packet_t *submit,
                               const pcap_packet_t *complete)
{
    uint8_t brequest;
    uint16_t wvalue;
    pcap_control_txn_t *txn;
    const uint8_t *data;
    size_t data_len;

    data = (complete != NULL) ? complete->data : NULL;
    data_len = (complete != NULL) ? complete->data_len : 0;
    brequest = submit->setup[1];
    wvalue = (uint16_t)submit->setup[2] | ((uint16_t)submit->setup[3] << 8);

    if (brequest == 0x06 && data_len > 0)
    {
        record_descriptor(st->descriptors, (uint8_t)(wvalue >> 8),
                          (uint8_t)(wvalue & 0xff), data, data_len);
    }

    txn = pcap_control_txn_new(submit->setup, data, data_len);
    g_queue_push_tail(st->control_queue, txn);
}

static void pcap_backend_index_trace(pcap_backend_state_t *st)
{
    size_t count;
    GHashTable *pending_submits;

    count = pcap_trace_count(st->trace);
    pending_submits = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                            g_free, NULL);

    for (size_t i = 0; i < count; i++)
    {
        const pcap_packet_t *p = pcap_trace_get(st->trace, i);
        if (p == NULL)
        {
            continue;
        }

        if (p->xfer == PCAP_XFER_CONTROL)
        {
            if (p->event_type == 'S' && p->setup_valid)
            {
                guint64 *key = g_new(guint64, 1);
                *key = p->urb_id;
                g_hash_table_insert(pending_submits, key, (gpointer)p);
            }
            else if (p->event_type == 'C')
            {
                const pcap_packet_t *submit;
                submit = g_hash_table_lookup(pending_submits, &p->urb_id);
                if (submit != NULL)
                {
                    index_control_pair(st, submit, p);
                    g_hash_table_remove(pending_submits, &p->urb_id);
                }
            }
        }
        else if (p->xfer == PCAP_XFER_INT || p->xfer == PCAP_XFER_BULK)
        {
            if (p->dir == PCAP_DIR_IN && p->event_type == 'C' && p->data_len > 0)
            {
                push_ep_payload(st->ep_in_queues, p->endpoint, p->data, p->data_len);
            }
            else if (p->dir == PCAP_DIR_OUT && p->event_type == 'S' && p->data_len > 0)
            {
                push_ep_payload(st->ep_out_queues, p->endpoint, p->data, p->data_len);
            }
        }
    }

    g_hash_table_destroy(pending_submits);
}

static GByteArray *lookup_cached_descriptor(GHashTable *descriptors,
                                            uint16_t value)
{
    uint8_t dtype;
    uint8_t dindex;
    guint desc_key;

    dtype = (uint8_t)(value >> 8);
    dindex = (uint8_t)(value & 0xff);
    desc_key = ((guint)dtype << 8) | dindex;

    return g_hash_table_lookup(descriptors, GUINT_TO_POINTER(desc_key));
}

static pcap_control_txn_t *pop_matching_control_txn(GQueue *queue,
                                                    const usb_control_packet_header_t *setup)
{
    GList *l;

    for (l = queue->head; l != NULL; l = l->next)
    {
        pcap_control_txn_t *cand;

        cand = (pcap_control_txn_t *)l->data;
        if (cand->setup[0] == setup->requesttype &&
            cand->setup[1] == setup->request &&
            ((uint16_t)cand->setup[2] | ((uint16_t)cand->setup[3] << 8)) == setup->value &&
            ((uint16_t)cand->setup[4] | ((uint16_t)cand->setup[5] << 8)) == setup->index)
        {
            g_queue_delete_link(queue, l);
            return cand;
        }
    }
    return NULL;
}

static int pb_get_descriptor(usb_device_t *dev, uint8_t type, uint8_t index,
                             uint16_t lang, const uint8_t **out, uint16_t *len)
{
    pcap_backend_state_t *st;
    GByteArray *buf;
    guint key;

    (void)lang;
    if (dev == NULL || dev->state == NULL)
    {
        return -1;
    }

    st = (pcap_backend_state_t *)dev->state;
    key = ((guint)type << 8) | index;
    buf = g_hash_table_lookup(st->descriptors, GUINT_TO_POINTER(key));
    if (buf == NULL)
    {
        LOG_DEBUG("pcap_backend: descriptor not found type=0x%02x index=%u", type, index);
        return -1;
    }

    if (out != NULL)
    {
        *out = buf->data;
    }
    if (len != NULL)
    {
        *len = (uint16_t)buf->len;
    }
    return 0;
}

static int handle_control_get_descriptor(pcap_backend_state_t *st,
                                         const usb_control_packet_header_t *setup,
                                         uint8_t *data,
                                         uint16_t *len)
{
    GByteArray *desc_buf;
    uint16_t copy_len;

    desc_buf = lookup_cached_descriptor(st->descriptors, setup->value);
    if (desc_buf == NULL)
    {
        return -1;
    }

    copy_len = (desc_buf->len < setup->length) ? (uint16_t)desc_buf->len : setup->length;
    if (copy_len > USB_VCR_BUF_SIZE)
    {
        copy_len = USB_VCR_BUF_SIZE;
    }
    if (data != NULL && copy_len > 0)
    {
        memcpy(data, desc_buf->data, copy_len);
    }
    if (len != NULL)
    {
        *len = copy_len;
    }
    return 0;
}

static int pb_control(usb_device_t *dev,
                      const usb_control_packet_header_t *setup,
                      uint8_t *data, uint16_t *len)
{
    pcap_backend_state_t *st;
    pcap_control_txn_t *txn;
    uint16_t copy_len;

    if (dev == NULL || dev->state == NULL || setup == NULL)
    {
        return -1;
    }

    st = (pcap_backend_state_t *)dev->state;
    if (setup->request == USB_REQ_GET_DESCRIPTOR)
    {
        if (handle_control_get_descriptor(st, setup, data, len) == 0)
        {
            return 0;
        }
    }

    txn = pop_matching_control_txn(st->control_queue, setup);
    if (txn == NULL)
    {
        LOG_DEBUG("pcap_backend: unhandled control setup req=0x%02x type=0x%02x val=0x%04x idx=0x%04x",
                  setup->request, setup->requesttype, setup->value, setup->index);
        return -1;
    }

    copy_len = 0;
    if (txn->reply_data != NULL && txn->reply_data->len > 0)
    {
        copy_len = (txn->reply_data->len < setup->length)
                     ? (uint16_t)txn->reply_data->len
                     : setup->length;
        if (copy_len > USB_VCR_BUF_SIZE)
        {
            copy_len = USB_VCR_BUF_SIZE;
        }
        if (data != NULL && copy_len > 0)
        {
            memcpy(data, txn->reply_data->data, copy_len);
        }
    }

    if (len != NULL)
    {
        *len = copy_len;
    }

    pcap_control_txn_free(txn);
    return 0;
}

static int pb_ep_in(usb_device_t *dev, uint8_t ep, uint8_t *buf, size_t cap)
{
    pcap_backend_state_t *st;
    GQueue *q;
    GByteArray *arr;
    size_t copy_len;

    if (dev == NULL || dev->state == NULL)
    {
        return -1;
    }

    st = (pcap_backend_state_t *)dev->state;
    q = g_hash_table_lookup(st->ep_in_queues, GUINT_TO_POINTER((guint)ep));
    if (q == NULL || g_queue_is_empty(q))
    {
        return 0;   /* NAK */
    }

    arr = (GByteArray *)g_queue_pop_head(q);
    copy_len = (arr->len < cap) ? arr->len : cap;
    if (buf != NULL && copy_len > 0)
    {
        memcpy(buf, arr->data, copy_len);
    }
    g_byte_array_unref(arr);

    return (int)copy_len;
}

static int pb_ep_out(usb_device_t *dev, uint8_t ep, const uint8_t *buf, size_t len)
{
    pcap_backend_state_t *st;
    GQueue *q;
    GByteArray *expected;

    if (dev == NULL || dev->state == NULL)
    {
        return -1;
    }

    st = (pcap_backend_state_t *)dev->state;
    q = g_hash_table_lookup(st->ep_out_queues, GUINT_TO_POINTER((guint)ep));
    if (q == NULL || g_queue_is_empty(q))
    {
        LOG_WARN("pcap_backend: unexpected OUT packet on ep 0x%02x (%zu bytes)", ep, len);
        return -1;
    }

    expected = (GByteArray *)g_queue_pop_head(q);
    if (len != expected->len || memcmp(buf, expected->data, len) != 0)
    {
        LOG_ERR("pcap_backend: OUT mismatch on ep 0x%02x: expected %u bytes, got %zu bytes",
                ep, expected->len, len);
        g_byte_array_unref(expected);
        return -1;
    }

    LOG_DEBUG("pcap_backend: OUT payload matched on ep 0x%02x (%zu bytes)", ep, len);
    g_byte_array_unref(expected);
    return 0;
}

static void pb_tick(usb_device_t *dev)
{
    (void)dev;
}

static const usb_device_ops_t pcap_ops = {
    .name           = "pcap",
    .get_descriptor = pb_get_descriptor,
    .control        = pb_control,
    .ep_in          = pb_ep_in,
    .ep_out         = pb_ep_out,
    .tick           = pb_tick,
};

void pcap_backend_destroy(usb_device_t *dev)
{
    pcap_backend_state_t *st;

    if (dev == NULL)
    {
        return;
    }

    st = (pcap_backend_state_t *)dev->state;
    if (st != NULL)
    {
        if (st->descriptors != NULL)
        {
            g_hash_table_destroy(st->descriptors);
        }
        if (st->control_queue != NULL)
        {
            g_queue_free_full(st->control_queue, pcap_control_txn_free);
        }
        if (st->ep_in_queues != NULL)
        {
            g_hash_table_destroy(st->ep_in_queues);
        }
        if (st->ep_out_queues != NULL)
        {
            g_hash_table_destroy(st->ep_out_queues);
        }
        if (st->trace != NULL)
        {
            pcap_trace_free(st->trace);
        }
        g_free(st);
    }
    g_free(dev);
}

usb_device_t *pcap_backend_create(const char *path)
{
    pcap_trace_t *trace;
    pcap_backend_state_t *st;
    usb_device_t *dev;

    trace = pcap_trace_load(path);
    if (!trace)
    {
        LOG_ERR("pcap_backend: failed to load capture: %s", path);
        return NULL;
    }

    st = g_new0(pcap_backend_state_t, 1);
    dev = g_new0(usb_device_t, 1);

    st->trace = trace;
    st->descriptors = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                            NULL, (GDestroyNotify)g_byte_array_unref);
    st->control_queue = g_queue_new();
    st->ep_in_queues = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                             NULL, ep_queue_free);
    st->ep_out_queues = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                              NULL, ep_queue_free);

    pcap_backend_index_trace(st);

    dev->ops = &pcap_ops;
    dev->state = st;
    return dev;
}
