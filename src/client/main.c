#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

#include "connection.h"
#include "usb_core.h"
#include "log.h"

/* ------------------------------------------------------------------ *
 * main                                                                *
 * ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    const char *host = "localhost";
    const char *port = "1337";

    // Need to replace this argument parser to something more structured.
    if (argc == 3) {
        host = argv[1];
        port = argv[2];
    } else if (argc != 1) {
        LOG_ERR("Usage: %s [host port] (default: localhost 1337)", argv[0]);
        return EXIT_FAILURE;
    }

    /* 1. Connect */
    LOG_DEBUG("Connecting to %s:%s ...", host, port);
    int sock = connect_to_server(host, port);
    if (sock < 0) {
        LOG_WARN("Connection failed");
        return EXIT_FAILURE;
    }
    LOG_INFO("Connected!");

    /* 2. Receive the server's hello */
    LOG_DEBUG("Waiting for server hello ...");
    int type = receive_and_print_packet(sock);
    if (type != RT_HELLO) {
        LOG_WARN("Expected hello (type 0), got type %d", type);
        close(sock);
        return EXIT_FAILURE;
    }
    LOG_INFO("Server hello received!");

    /* 3. Send our hello */
    uint8_t pkt[USB_REDIR_BUF_SIZE];
    size_t pkt_len = build_hello_packet(pkt);
    LOG_OUT("Sending our hello packet (%zu bytes) ...", pkt_len);
    if (send(sock, pkt, pkt_len, 0) < 0) {
        perror("send");
        close(sock);
        return EXIT_FAILURE;
    }
    LOG_INFO("Hello sent!");
    LOG_INFO("    *** Hello, USB-redir world! ***");

    pkt_len = build_interface_info_pkt(pkt);
    LOG_OUT("Sending our interface info packet (%zu bytes) ...", pkt_len);
    if (send(sock, pkt, pkt_len, 0) < 0) {
        perror("send");
        close(sock);
        return EXIT_FAILURE;
    }

    pkt_len = build_ep_info_pkt(pkt);
    LOG_OUT("Sending our EP info packet (%zu bytes) ...", pkt_len);
    if (send(sock, pkt, pkt_len, 0) < 0) {
        perror("send");
        close(sock);
        return EXIT_FAILURE;
    }

    pkt_len = build_connect_pkt(pkt);
    LOG_OUT("Sending our connect packet (%zu bytes) ...", pkt_len);
    if (send(sock, pkt, pkt_len, 0) < 0) {
        perror("send");
        close(sock);
        return EXIT_FAILURE;
    }

    /* 4. Done */
    close(sock);
    return EXIT_SUCCESS;
}
