#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "transport.h"
#include "usb_core.h"
#include "device.h"
#include "log.h"

/* Backend registry: name -> factory. Add mass-storage, uvc, ccid here. */
static const struct {
    const char *name;
    usb_device_t *(*create)(void);
} registry[] = {
    { "hid-keyboard", hid_keyboard_create },
};

static usb_device_t *create_device(const char *name)
{
    for (size_t i = 0; i < sizeof(registry) / sizeof(registry[0]); i++)
        if (strcmp(name, registry[i].name) == 0)
            return registry[i].create();
    return NULL;
}

static void usage(const char *prog)
{
    LOG_ERR("Usage: %s [host [port [device]]]", prog);
    LOG_ERR("  defaults: localhost 1337 hid-keyboard");
}

int main(int argc, char *argv[])
{
    const char *host = "localhost";
    const char *port = "1337";
    const char *device = "hid-keyboard";

    /* Up to three positionals. */
    const char *pos[3];
    int npos = 0;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            usage(argv[0]);
            return EXIT_FAILURE;
        } else if (npos < 3) {
            pos[npos++] = argv[i];
        } else {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (npos >= 1) host = pos[0];
    if (npos >= 2) port = pos[1];
    if (npos >= 3) device = pos[2];

    LOG_DEBUG("Connecting to %s:%s ...", host, port);
    int sock = connect_to_server(host, port);
    if (sock < 0) {
        LOG_WARN("Connection failed");
        return EXIT_FAILURE;
    }
    LOG_INFO("Connected!");

    usb_device_t *dev = create_device(device);
    if (!dev) {
        LOG_ERR("unknown device backend: %s", device);
        close(sock);
        return EXIT_FAILURE;
    }

    LOG_INFO("Emulating device: %s", device);
    int rc = usb_core_run(sock, dev);

    close(sock);
    LOG_INFO("Session ended (rc=%d)", rc);
    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
