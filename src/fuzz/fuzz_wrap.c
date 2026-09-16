#include <stdlib.h>
#include <string.h>

#include "fuzz_wrap.h"
#include "protocol.h"
#include "log.h"

typedef struct {
    usb_device_t *base;
    fuzz_rng_t    rng;
    fuzz_cfg_t    cfg;
    uint8_t       scratch[USB_REDIR_BUF_SIZE];  /* mutated descriptor storage */
} fuzz_state_t;

static int fw_get_descriptor(usb_device_t *dev, uint8_t type, uint8_t index,
                             uint16_t lang, const uint8_t **out, uint16_t *len)
{
    fuzz_state_t *fs = dev->state;
    const uint8_t *base_desc;
    uint16_t base_len;

    if (fs->base->ops->get_descriptor(fs->base, type, index, lang,
                                      &base_desc, &base_len) < 0)
        return -1;

    if (!fs->cfg.fuzz_descriptors) {
        *out = base_desc;
        *len = base_len;
        return 0;
    }

    /* Copy into mutable scratch, then mutate in place. */
    size_t n = base_len < sizeof(fs->scratch) ? base_len : sizeof(fs->scratch);
    memcpy(fs->scratch, base_desc, n);
    n = fuzz_mutate_descriptor(&fs->rng, &fs->cfg, fs->scratch, n,
                               sizeof(fs->scratch));

    LOG_DEBUG("fuzz: descriptor type=0x%02x %u -> %zu bytes", type, base_len, n);
    *out = fs->scratch;
    *len = (uint16_t)n;
    return 0;
}

static int fw_control(usb_device_t *dev,
                      const usb_control_packet_header_t *setup,
                      uint8_t *data, uint16_t *len)
{
    fuzz_state_t *fs = dev->state;
    if (!fs->base->ops->control)
        return -1;

    int rc = fs->base->ops->control(fs->base, setup, data, len);
    if (rc == 0 && fs->cfg.fuzz_control)
        *len = (uint16_t)fuzz_mutate_bytes(&fs->rng, &fs->cfg, data, *len,
                                           USB_REDIR_BUF_SIZE);
    return rc;
}

static int fw_ep_in(usb_device_t *dev, uint8_t ep, uint8_t *buf, size_t cap)
{
    fuzz_state_t *fs = dev->state;
    if (!fs->base->ops->ep_in)
        return 0;

    int n = fs->base->ops->ep_in(fs->base, ep, buf, cap);
    if (n > 0 && fs->cfg.fuzz_ep_in)
        n = (int)fuzz_mutate_bytes(&fs->rng, &fs->cfg, buf, (size_t)n, cap);
    return n;
}

static int fw_ep_out(usb_device_t *dev, uint8_t ep, const uint8_t *buf, size_t len)
{
    fuzz_state_t *fs = dev->state;
    if (fs->base->ops->ep_out)
        return fs->base->ops->ep_out(fs->base, ep, buf, len);
    return 0;
}

static void fw_tick(usb_device_t *dev)
{
    fuzz_state_t *fs = dev->state;
    if (fs->base->ops->tick)
        fs->base->ops->tick(fs->base);
}

static const usb_device_ops fuzz_ops = {
    .name           = "fuzz",
    .get_descriptor = fw_get_descriptor,
    .control        = fw_control,
    .ep_in          = fw_ep_in,
    .ep_out         = fw_ep_out,
    .tick           = fw_tick,
};

usb_device_t *fuzz_wrap(usb_device_t *base, const fuzz_cfg_t *cfg)
{
    usb_device_t *dev = calloc(1, sizeof(*dev));
    fuzz_state_t *fs = calloc(1, sizeof(*fs));
    if (!dev || !fs) {
        free(dev); free(fs);
        LOG_ERR("fuzz_wrap: out of memory");
        return NULL;
    }
    fs->base = base;
    fs->cfg = *cfg;
    fuzz_rng_seed(&fs->rng, cfg->seed);
    dev->ops = &fuzz_ops;
    dev->state = fs;
    return dev;
}
