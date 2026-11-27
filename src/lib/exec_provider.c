#include "exec_provider.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

struct exec_provider {
    char        *command;
    char        *working_dir;
    GPid         pid;
    int          stdin_fd;
    int          stdout_fd;
    uint8_t      ep_in;
    uint8_t      ep_out;
};

static void set_nonblocking(int fd)
{
    if (fd < 0)
        return;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

exec_provider_t *exec_provider_new(const char *command, const char *working_dir,
                                   uint8_t ep_in, uint8_t ep_out)
{
    if (command == NULL)
        return NULL;

    GError *error = NULL;
    gchar **argv = NULL;
    if (!g_shell_parse_argv(command, NULL, &argv, &error))
    {
        LOG_ERR("exec_provider: failed to parse command '%s': %s", command,
                error ? error->message : "unknown error");
        g_clear_error(&error);
        return NULL;
    }

    GPid pid = 0;
    int stdin_fd = -1;
    int stdout_fd = -1;

    gboolean spawned = g_spawn_async_with_pipes(
        working_dir,
        argv,
        NULL,
        G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
        NULL,
        NULL,
        &pid,
        &stdin_fd,
        &stdout_fd,
        NULL,
        &error
    );

    g_strfreev(argv);

    if (!spawned)
    {
        LOG_ERR("exec_provider: failed to spawn process '%s': %s", command,
                error ? error->message : "unknown error");
        g_clear_error(&error);
        return NULL;
    }

    set_nonblocking(stdin_fd);
    set_nonblocking(stdout_fd);

    exec_provider_t *ep = g_new0(exec_provider_t, 1);
    ep->command = g_strdup(command);
    ep->working_dir = g_strdup(working_dir);
    ep->pid = pid;
    ep->stdin_fd = stdin_fd;
    ep->stdout_fd = stdout_fd;
    ep->ep_in = (ep_in != 0) ? ep_in : 0x81;
    ep->ep_out = (ep_out != 0) ? ep_out : 0x01;

    LOG_INFO("exec_provider: spawned '%s' (pid=%d) ep_in=0x%02x ep_out=0x%02x",
             command, (int)pid, ep->ep_in, ep->ep_out);

    return ep;
}

void exec_provider_free(exec_provider_t *ep)
{
    if (ep == NULL)
        return;

    if (ep->stdin_fd >= 0)
    {
        close(ep->stdin_fd);
        ep->stdin_fd = -1;
    }
    if (ep->stdout_fd >= 0)
    {
        close(ep->stdout_fd);
        ep->stdout_fd = -1;
    }

    if (ep->pid > 0)
    {
        kill(ep->pid, SIGTERM);
        waitpid(ep->pid, NULL, WNOHANG);
        g_spawn_close_pid(ep->pid);
        ep->pid = 0;
    }

    g_free(ep->command);
    g_free(ep->working_dir);
    g_free(ep);
}

uint8_t exec_provider_get_ep_in(exec_provider_t *ep)
{
    return ep ? ep->ep_in : 0x81;
}

uint8_t exec_provider_get_ep_out(exec_provider_t *ep)
{
    return ep ? ep->ep_out : 0x01;
}

static int exec_produce_in(void *ctx, uint8_t ep, uint8_t *buf, size_t cap)
{
    exec_provider_t *prov = (exec_provider_t *)ctx;
    (void)ep;

    if (prov == NULL || prov->stdout_fd < 0 || buf == NULL || cap == 0)
        return -1;

    ssize_t n = read(prov->stdout_fd, buf, cap);
    if (n > 0)
    {
        return (int)n;
    }
    else if (n < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return 0; /* NAK: no data ready */
        }
        return -1;
    }
    return 0; /* EOF */
}

static int exec_consume_out(void *ctx, uint8_t ep, const uint8_t *buf, size_t len, GQueue *in_queue)
{
    exec_provider_t *prov = (exec_provider_t *)ctx;
    (void)ep;
    (void)in_queue;

    if (prov == NULL || prov->stdin_fd < 0 || buf == NULL || len == 0)
        return -1;

    ssize_t written = write(prov->stdin_fd, buf, len);
    if (written < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
    return 0;
}

static void exec_free_op(void *ctx)
{
    exec_provider_free((exec_provider_t *)ctx);
}

static const flow_endpoint_ops_t exec_endpoint_ops = {
    .produce_in = exec_produce_in,
    .consume_out = exec_consume_out,
    .tick = NULL,
    .free = exec_free_op,
};

const flow_endpoint_ops_t *exec_provider_get_ops(void)
{
    return &exec_endpoint_ops;
}
