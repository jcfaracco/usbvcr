#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "transport.h"
#include "usb_core.h"
#include "device.h"
#include "yaml_backend.h"
#include "fuzz_wrap.h"
#include "record.h"
#include "log.h"

static void usage(const char *prog)
{
    LOG_ERR("Usage: %s [opts] --yaml <file> [host [port]]", prog);
    LOG_ERR("  --yaml FILE   canonical device project (devices/ flows/ ...)");
    LOG_ERR("  --debug       verbose per-packet logging");
    LOG_ERR("  --fuzz        enable descriptor/endpoint mutation");
    LOG_ERR("  --seed N      fixed PRNG seed (implies --fuzz; for repro)");
    LOG_ERR("  --record FILE log every sent frame to FILE");
    LOG_ERR("  --replay FILE resend frames from FILE, then exit");
    LOG_ERR("  defaults: localhost 1337");
}

int main(int argc, char *argv[])
{
    const char *host = "localhost";
    const char *port = "1337";
    const char *devices_yaml = NULL;
    const char *record_path = NULL;
    const char *replay_path = NULL;
    int do_fuzz = 0;
    int have_seed = 0;
    uint64_t seed = 0;

    /* Options, then up to two positionals (host, port). */
    const char *pos[2];
    int npos = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0) {
            current_log_level = LOG_LEVEL_DEBUG;
        } else if (strcmp(argv[i], "--yaml") == 0 && i + 1 < argc) {
            devices_yaml = argv[++i];
        } else if (strcmp(argv[i], "--fuzz") == 0) {
            do_fuzz = 1;
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = strtoull(argv[++i], NULL, 0);
            have_seed = 1;
            do_fuzz = 1;
        } else if (strcmp(argv[i], "--record") == 0 && i + 1 < argc) {
            record_path = argv[++i];
        } else if (strcmp(argv[i], "--replay") == 0 && i + 1 < argc) {
            replay_path = argv[++i];
        } else if (argv[i][0] == '-') {
            usage(argv[0]);
            return EXIT_FAILURE;
        } else if (npos < 2) {
            pos[npos++] = argv[i];
        } else {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (npos >= 1) host = pos[0];
    if (npos >= 2) port = pos[1];

    LOG_DEBUG("Connecting to %s:%s ...", host, port);
    int sock = connect_to_server(host, port);
    if (sock < 0) {
        LOG_WARN("Connection failed");
//        return EXIT_FAILURE;
    }
//    LOG_INFO("Connected!");

    /* Replay mode: just resend a recorded run and exit. */
    if (replay_path) {
        int rc = replay_run(sock, replay_path);
        close(sock);
        return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (!devices_yaml) {
        LOG_ERR("no device: pass --yaml <dir>");
        usage(argv[0]);
        close(sock);
        return EXIT_FAILURE;
    }
    usb_device_t *dev = yaml_backend_create(devices_yaml);
    if (!dev) {
        LOG_ERR("failed to load device project: %s", devices_yaml);
        close(sock);
        return EXIT_FAILURE;
    }

    if (do_fuzz) {
        if (!have_seed)
            seed = ((uint64_t)time(NULL) << 16) ^ (uint64_t)getpid();
        LOG_INFO("FUZZ seed=%llu  (reproduce: --seed %llu)",
                 (unsigned long long)seed, (unsigned long long)seed);

        fuzz_cfg_t cfg = {
            .seed = seed,
            .prob = 4,              /* ~25% of mutable items */
            .fuzz_descriptors = 1,
            .fuzz_ep_in = 1,
            .fuzz_control = 0,
        };
        dev = fuzz_wrap(dev, &cfg);
        if (!dev) {
            close(sock);
            return EXIT_FAILURE;
        }
    }

    if (record_path && record_open(record_path) < 0) {
        close(sock);
        return EXIT_FAILURE;
    }

    LOG_INFO("Emulating device: %s", devices_yaml);
    int rc = usb_core_run(sock, dev);

    record_close();
    close(sock);
    LOG_INFO("Session ended (rc=%d)", rc);
    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
