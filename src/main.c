#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <glib.h>

#include "transport.h"
#include "protocol.h"
#include "bytes.h"
#include "device.h"
#include "yaml_backend.h"
#include "pcap_backend.h"
#include "fuzz_wrap.h"
#include "pcap/record.h"
#include "version.h"
#include "log.h"
#include "qemu_engine.h"
#include "local_engine.h"


typedef struct {
    gchar *host;
    gchar *port;
    gchar *yaml;
    gchar *pcap;
    gchar *record_path;
    gchar *udc;
    gboolean debug;
    gboolean version;
    gboolean do_fuzz;
    gboolean have_seed;
    gboolean local;
    uint64_t seed;
} GAppConfig;

/* Instance name for the local gadget (ConfigFS/FunctionFS dir names): the
 * YAML project's `name:` field when configured, or `usbvcr_<vid>_<pid>`
 * derived from the device descriptor for a pcap replay. Falls back to the
 * plain "usbvcr" default when neither is available. Caller g_free()s. */
static gchar *local_instance_name(const GAppConfig *config, usb_device_t *dev)
{
    if (config->yaml) {
        const char *name = yaml_backend_get_name(dev);
        return g_strdup(name ? name : "usbvcr");
    }

    if (config->pcap) {
        const uint8_t *desc = NULL;
        uint16_t len = 0;
        if (dev->ops->get_descriptor(dev, USB_DESC_DEVICE, 0, 0, &desc, &len) == 0 &&
            len >= USB_DD_idProduct + 2) {
            uint16_t vid = get_u16le(desc + USB_DD_idVendor);
            uint16_t pid = get_u16le(desc + USB_DD_idProduct);
            return g_strdup_printf("usbvcr_%04x_%04x", vid, pid);
        }
    }

    return g_strdup("usbvcr");
}

int main(int argc, char *argv[])
{
    GOptionContext *context;
    GAppConfig config = {
        .host = "localhost",
        .port = "1337",
        .yaml = NULL,
        .pcap = NULL,
        .record_path = NULL,
        .udc = NULL,
        .debug = FALSE,
        .version = FALSE,
        .do_fuzz = FALSE,
        .have_seed = FALSE,
        .local = FALSE,
        .seed = 0,
    };
    GError *error = NULL;
    int rc = 0;
    int sock = -1;

    GOptionEntry entries[] = {
        { "debug", 'D', 0, G_OPTION_ARG_NONE, &config.debug, "enable debugging logs.", NULL },
        { "host", 'H', 0, G_OPTION_ARG_STRING, &config.host, "address of the host to connect (default=localhost).", "HOST" },
        { "port", 'P', 0, G_OPTION_ARG_STRING, &config.port, "port of the host to connect (default=1337).", "PORT" },
        { "yaml", 0, 0, G_OPTION_ARG_FILENAME, &config.yaml, "canonical device project (devices/ flows/ ...).", "FILE" },
        { "pcap", 0, 0, G_OPTION_ARG_FILENAME, &config.pcap, "pcap file to replay USB behaviour.", "FILE" },
        { "record", 'r', 0, G_OPTION_ARG_FILENAME, &config.record_path, "record live USB transfers to a pcap/pcapng file (format inferred from extension).", "FILE" },
        { "local", 0, 0, G_OPTION_ARG_NONE, &config.local, "run the device locally against a Linux USB gadget (FunctionFS/ConfigFS) instead of QEMU.", NULL },
        { "udc", 0, 0, G_OPTION_ARG_STRING, &config.udc, "UDC to bind the local gadget to, with --local (default: auto-detect).", "UDC" },
        { "version", 'V', 0, G_OPTION_ARG_NONE, &config.version, "Show version and exit.", NULL },
        { NULL }
    };

    // Create the context with a usage summary
    context = g_option_context_new("- USB VCR: emulate or replay USB device behaviours");
    g_option_context_add_main_entries(context, entries, NULL);

    /* Pick a backend: a usbmon pcap/pcapng capture (replay a real device) or a
     * canonical YAML device project (emulate). Both feed the same engine. */
    usb_device_t *dev = NULL;
    usb_device_t *pcap_dev = NULL;
    gchar *local_name = NULL;

    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_print("Error parsing options: %s\n", error->message);
        rc = -1;
        goto end;
    }

    if (config.version) {
        g_print("USB VCR version " USB_VCR_VERSION "\n");
        goto end;   /* rc == 0 */
    }

    if (config.debug)
        current_log_level = LOG_LEVEL_DEBUG;

    if (config.pcap) {
        dev = pcap_backend_create(config.pcap);
        if (!dev) {
            LOG_ERR("failed to load pcap capture: %s", config.pcap);
            rc = -1;
            goto end;
        }
        pcap_dev = dev;
    } else if (config.yaml) {
        dev = yaml_backend_create(config.yaml);
        if (!dev) {
            LOG_ERR("failed to load device project: %s", config.yaml);
            rc = -1;
            goto end;
        }
    } else {
        LOG_ERR("no device: pass --yaml <FILE> or --pcap <FILE>");
        rc = -1;
        goto end;
    }

    /* Computed before fuzz_wrap, which replaces dev->state with its own —
     * the backend-specific accessors below would otherwise read garbage. */
    if (config.local)
        local_name = local_instance_name(&config, dev);

    if (config.do_fuzz) {
        if (!config.have_seed)
            config.seed = ((uint64_t)time(NULL) << 16) ^ (uint64_t)getpid();
        LOG_INFO("FUZZ seed=%llu  (reproduce: --seed %llu)",
                 (unsigned long long)config.seed,
                 (unsigned long long)config.seed);

        fuzz_cfg_t cfg = {
            .seed = config.seed,
            .prob = 4,              /* ~25% of mutable items */
            .fuzz_descriptors = 1,
            .fuzz_ep_in = 1,
            .fuzz_control = 0,
        };
        dev = fuzz_wrap(dev, &cfg);
        if (!dev) {
            rc = -1;
            goto end;
        }
    }

    if (config.record_path && record_open(config.record_path) < 0) {
        rc = -1;
        goto end;
    }

    LOG_INFO("Running device: %s", config.pcap ? config.pcap : config.yaml);

    if (config.local) {
        LOG_INFO("Running locally on UDC %s (gadget name '%s')",
                 config.udc ? config.udc : "(auto-detect)", local_name);
        rc = local_engine_run(dev, local_name, config.udc);
    } else {
        LOG_DEBUG("Connecting to %s:%s ...", config.host, config.port);
        sock = connect_to_server(config.host, config.port);
        if (sock < 0) {
            LOG_ERR("Connection failed");
            rc = -1;
            goto end;
        }
        LOG_INFO("Connected!");
        rc = qemu_engine_run(sock, dev);
    }

    LOG_INFO("Session ended (rc=%d)", rc);

end:
    g_free(local_name);
    record_close();
    if (sock >= 0)
        close(sock);
    if (pcap_dev)
        pcap_backend_destroy(pcap_dev);
    if (error)
        g_clear_error(&error);
    if (context)
        g_option_context_free(context);
    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
