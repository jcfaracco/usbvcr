#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>

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
        fprintf(stderr, "Usage: %s [host port]\n"
                        "Default: localhost 1337\n", argv[0]);
        return EXIT_FAILURE;
    }

    /* 1. Connect */
    printf("[*] Connecting to %s:%s ...\n", host, port);
    int sock = connect_to_server(host, port);
    if (sock < 0) {
        fprintf(stderr, "[!] Connection failed\n");
        return EXIT_FAILURE;
    }
    printf("[+] Connected!\n\n");

    /* 2. Receive the server's hello */
    printf("[*] Waiting for server hello ...\n");
    int type = receive_and_print_packet(sock);
    if (type != RT_HELLO) {
        fprintf(stderr, "[!] Expected hello (type 0), got type %d\n", type);
        close(sock);
        return EXIT_FAILURE;
    }
    printf("[+] Server hello received!\n\n");

    /* 3. Send our hello */
    uint8_t pkt[256];
    size_t pkt_len = build_hello_packet(pkt);
    printf("[>] Sending our hello (%zu bytes) ...\n", pkt_len);
    if (send(sock, pkt, pkt_len, 0) < 0) {
        perror("send");
        close(sock);
        return EXIT_FAILURE;
    }
    printf("[+] Hello sent!\n\n");

    printf("    *** Hello, USB-redir world! ***\n\n");

    /* 4. Done */
    close(sock);
    return EXIT_SUCCESS;
}
