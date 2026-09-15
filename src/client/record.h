#ifndef USBREDIR_CLIENT_RECORD_H
#define USBREDIR_CLIENT_RECORD_H

/* Open `path` for recording sent frames (wires it into the transport).
 * Returns 0, or -1 on error. */
int record_open(const char *path);
void record_close(void);

/* Replay a recorded frame log to `sock`, resending each frame verbatim.
 * Deterministic reproduction of a crashing run. Returns 0, or -1. */
int replay_run(int sock, const char *path);

#endif
