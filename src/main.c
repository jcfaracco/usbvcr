#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <glib.h>

#include "transport.h"
#include "usb_core.h"
#include "device.h"
#include "yaml_backend.h"
#include "pcap_backend.h"
#include "fuzz_wrap.h"
#include "pcap/record.h"
#include "version.h"
#include "log.h"


typedef struct {
    gchar *host;
    gchar *port;
    gchar *yaml;
    gchar *pcap;
    gchar *record_path;
    gchar *replay_path;
    gboolean debug;
    gboolean version;
    gboolean do_record;
    gboolean do_fuzz;
    gboolean have_seed;
    uint64_t seed;
} GAppConfig;

int main(int argc, char *argv[])
{
    GOptionContext *context;
    GAppConfig config = {
        .host = "localhost",
        .port = "1337",
        .yaml = NULL,
        .pcap = NULL,
        .record_path = NULL,
        .debug = FALSE,
        .version = FALSE,
        .do_record = FALSE,
        .do_fuzz = FALSE,
        .have_seed = FALSE,
        .seed = 0,
    };
    GError *error = NULL;
    int rc = 0;

    GOptionEntry entries[] = {
        { "debug", 'D', 0, G_OPTION_ARG_NONE, &config.debug, "enable debugging logs.", NULL },
        { "host", 'H', 0, G_OPTION_ARG_STRING, &config.host, "address of the host to connect (default=localhost).", "HOST" },
        { "port", 'P', 0, G_OPTION_ARG_STRING, &config.port, "port of the host to connect (default=1337).", "PORT" },
        { "yaml", 0, 0, G_OPTION_ARG_FILENAME, &config.yaml, "canonical device project (devices/ flows/ ...).", "FILE" },
        { "pcap", 0, 0, G_OPTION_ARG_FILENAME, &config.pcap, "pcap file to replay USB behaviour.", "FILE" },
        { "version", 'V', 0, G_OPTION_ARG_NONE, &config.version, "Show version and exit.", NULL },
        { NULL }
    };

    // Create the context with a usage summary
    context = g_option_context_new("- USB VCR: emulate or replay USB device behaviours");
    g_option_context_add_main_entries(context, entries, NULL);

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

    LOG_DEBUG("Connecting to %s:%s ...", config.host, config.port);
    int sock = connect_to_server(config.host, config.port);
    if (sock < 0) {
        LOG_ERR("Connection failed");
        rc = -1;
        goto end;
    }
    LOG_INFO("Connected!");

    /* Pick a backend: a usbmon pcap/pcapng capture (replay a real device) or a
     * canonical YAML device project (emulate). Both feed the same engine. */
    usb_device_t *dev = NULL;
    if (config.pcap) {
        dev = pcap_backend_create(config.pcap);
        if (!dev) {
            LOG_ERR("failed to load pcap capture: %s", config.pcap);
            close(sock);
            rc = -1;
            goto end;
        }
    } else if (config.yaml) {
        dev = yaml_backend_create(config.yaml);
        if (!dev) {
            LOG_ERR("failed to load device project: %s", config.yaml);
            close(sock);
            rc = -1;
            goto end;
        }
    } else {
        LOG_ERR("no device: pass --yaml <FILE> or --pcap <FILE>");
        close(sock);
        rc = -1;
        goto end;
    }

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
            close(sock);
            rc = -1;
            goto end;
        }
    }

    if (config.record_path && record_open(config.record_path) < 0) {
        close(sock);
        rc = -1;
        goto end;
    }

    LOG_INFO("Running device: %s", config.pcap ? config.pcap : config.yaml);
    rc = usb_core_run(sock, dev);

    record_close();
    close(sock);
    LOG_INFO("Session ended (rc=%d)", rc);

end:
    if (error)
        g_clear_error(&error);
    if (context)
        g_option_context_free(context);
    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
