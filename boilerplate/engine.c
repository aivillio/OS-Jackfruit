/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 1024
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    int stop_requested;
    char termination_reason[32];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
    int exit_code;
    int exit_signal;
    int log_read_fd;
    pthread_t producer_thread;
    int producer_started;
    void *child_stack;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

typedef struct {
    supervisor_ctx_t *ctx;
    container_record_t *record;
} log_producer_ctx_t;

int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item);
int child_fn(void *arg);
int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes);
int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid);
static const char *state_to_string(container_state_t state);

static volatile sig_atomic_t g_stop_requested = 0;
static volatile sig_atomic_t g_reap_requested = 0;
static volatile sig_atomic_t g_run_signal_requested = 0;

static void supervisor_term_handler(int signo)
{
    (void)signo;
    g_stop_requested = 1;
}

static void supervisor_sigchld_handler(int signo)
{
    (void)signo;
    g_reap_requested = 1;
}

static void run_client_term_handler(int signo)
{
    (void)signo;
    g_run_signal_requested = 1;
}

static int write_full(int fd, const void *buf, size_t len)
{
    const char *p = buf;

    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int read_full(int fd, void *buf, size_t len)
{
    char *p = buf;

    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static container_record_t *find_container_by_id(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *cur;

    for (cur = ctx->containers; cur != NULL; cur = cur->next) {
        if (strncmp(cur->id, id, CONTAINER_ID_LEN) == 0)
            return cur;
    }
    return NULL;
}

static container_record_t *find_container_by_pid(supervisor_ctx_t *ctx, pid_t pid)
{
    container_record_t *cur;

    for (cur = ctx->containers; cur != NULL; cur = cur->next) {
        if (cur->host_pid == pid)
            return cur;
    }
    return NULL;
}

static container_record_t *find_live_container_by_rootfs(supervisor_ctx_t *ctx,
                                                         const char *rootfs)
{
    container_record_t *cur;

    for (cur = ctx->containers; cur != NULL; cur = cur->next) {
        if ((cur->state == CONTAINER_STARTING || cur->state == CONTAINER_RUNNING) &&
            strncmp(cur->rootfs, rootfs, PATH_MAX) == 0)
            return cur;
    }

    return NULL;
}

static void *container_log_producer_thread(void *arg)
{
    log_producer_ctx_t *producer_ctx = arg;
    supervisor_ctx_t *ctx;
    container_record_t *rec;

    if (producer_ctx == NULL)
        return NULL;

    ctx = producer_ctx->ctx;
    rec = producer_ctx->record;
    free(producer_ctx);

    for (;;) {
        log_item_t item;
        ssize_t n;
        int fd;

        pthread_mutex_lock(&ctx->metadata_lock);
        fd = rec->log_read_fd;
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (fd < 0)
            break;

        n = read(fd, item.data, sizeof(item.data));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (n == 0)
            break;

        memset(&item.container_id, 0, sizeof(item.container_id));
        strncpy(item.container_id, rec->id, sizeof(item.container_id) - 1);
        item.length = (size_t)n;
        if (bounded_buffer_push(&ctx->log_buffer, &item) != 0)
            break;
    }

    pthread_mutex_lock(&ctx->metadata_lock);
    if (rec->log_read_fd >= 0) {
        close(rec->log_read_fd);
        rec->log_read_fd = -1;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    return NULL;
}

static int start_log_producer(supervisor_ctx_t *ctx, container_record_t *rec)
{
    log_producer_ctx_t *producer_ctx;
    int rc;

    producer_ctx = malloc(sizeof(*producer_ctx));
    if (producer_ctx == NULL)
        return -1;

    producer_ctx->ctx = ctx;
    producer_ctx->record = rec;

    rc = pthread_create(&rec->producer_thread,
                        NULL,
                        container_log_producer_thread,
                        producer_ctx);
    if (rc != 0) {
        free(producer_ctx);
        errno = rc;
        return -1;
    }

    rec->producer_started = 1;
    return 0;
}

static void join_log_producer(container_record_t *rec)
{
    if (rec->producer_started) {
        pthread_join(rec->producer_thread, NULL);
        rec->producer_started = 0;
    }
}

static void stop_log_producer(supervisor_ctx_t *ctx, container_record_t *rec)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    if (rec->log_read_fd >= 0) {
        close(rec->log_read_fd);
        rec->log_read_fd = -1;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    join_log_producer(rec);
}

static void stop_all_log_producers(supervisor_ctx_t *ctx)
{
    container_record_t *cur;

    pthread_mutex_lock(&ctx->metadata_lock);
    for (cur = ctx->containers; cur != NULL; cur = cur->next) {
        if (cur->log_read_fd >= 0) {
            close(cur->log_read_fd);
            cur->log_read_fd = -1;
        }
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    for (cur = ctx->containers; cur != NULL; cur = cur->next)
        join_log_producer(cur);
}

static void reap_children(supervisor_ctx_t *ctx)
{
    int status;
    pid_t pid;

    for (;;) {
        container_record_t *rec;

        pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0)
            break;

        pthread_mutex_lock(&ctx->metadata_lock);
        rec = find_container_by_pid(ctx, pid);
        if (rec != NULL) {
            if (WIFEXITED(status)) {
                rec->state = rec->stop_requested ? CONTAINER_STOPPED : CONTAINER_EXITED;
                rec->exit_code = WEXITSTATUS(status);
                rec->exit_signal = 0;
                snprintf(rec->termination_reason,
                         sizeof(rec->termination_reason),
                         "%s",
                         rec->stop_requested ? "stopped" : "exited");
            } else if (WIFSIGNALED(status)) {
                if (rec->stop_requested)
                    rec->state = CONTAINER_STOPPED;
                else
                    rec->state = CONTAINER_KILLED;
                rec->exit_code = -1;
                rec->exit_signal = WTERMSIG(status);

                if (rec->stop_requested) {
                    snprintf(rec->termination_reason,
                             sizeof(rec->termination_reason),
                             "%s",
                             "stopped");
                } else if (rec->exit_signal == SIGKILL) {
                    snprintf(rec->termination_reason,
                             sizeof(rec->termination_reason),
                             "%s",
                             "hard_limit_killed");
                } else {
                    snprintf(rec->termination_reason,
                             sizeof(rec->termination_reason),
                             "%s",
                             "killed");
                }
            } else {
                rec->state = CONTAINER_STOPPED;
                rec->exit_code = -1;
                rec->exit_signal = 0;
                snprintf(rec->termination_reason,
                         sizeof(rec->termination_reason),
                         "%s",
                         "stopped");
            }

            if (ctx->monitor_fd >= 0)
                (void)unregister_from_monitor(ctx->monitor_fd, rec->id, rec->host_pid);
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }
}

static int handle_start_like_request(supervisor_ctx_t *ctx,
                                     const control_request_t *req,
                                     control_response_t *resp)
{
    container_record_t *rec;
    child_config_t child_cfg;
    int pipefd[2];
    void *stack;
    char *stack_top;
    pid_t pid;

    if (req->container_id[0] == '\0' || req->rootfs[0] == '\0' || req->command[0] == '\0') {
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "missing id/rootfs/command");
        return -1;
    }

    pthread_mutex_lock(&ctx->metadata_lock);
    if (find_container_by_id(ctx, req->container_id) != NULL) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "container already exists: %s", req->container_id);
        return -1;
    }

    if (find_live_container_by_rootfs(ctx, req->rootfs) != NULL) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = -1;
        snprintf(resp->message,
                 sizeof(resp->message),
                 "rootfs already in use by live container: %s",
                 req->rootfs);
        return -1;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    rec = calloc(1, sizeof(*rec));
    if (rec == NULL) {
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "out of memory");
        return -1;
    }

    if (pipe(pipefd) < 0) {
        free(rec);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "pipe failed: %s", strerror(errno));
        return -1;
    }

    stack = malloc(STACK_SIZE);
    if (stack == NULL) {
        close(pipefd[0]);
        close(pipefd[1]);
        free(rec);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "stack allocation failed");
        return -1;
    }
    stack_top = (char *)stack + STACK_SIZE;

    memset(&child_cfg, 0, sizeof(child_cfg));
    strncpy(child_cfg.id, req->container_id, sizeof(child_cfg.id) - 1);
    strncpy(child_cfg.rootfs, req->rootfs, sizeof(child_cfg.rootfs) - 1);
    strncpy(child_cfg.command, req->command, sizeof(child_cfg.command) - 1);
    child_cfg.nice_value = req->nice_value;
    child_cfg.log_write_fd = pipefd[1];

    pid = clone(child_fn,
                stack_top,
                CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                &child_cfg);
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        free(stack);
        free(rec);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "clone failed: %s", strerror(errno));
        return -1;
    }

    close(pipefd[1]);

    memset(rec, 0, sizeof(*rec));
    strncpy(rec->id, req->container_id, sizeof(rec->id) - 1);
    strncpy(rec->rootfs, req->rootfs, sizeof(rec->rootfs) - 1);
    rec->host_pid = pid;
    rec->started_at = time(NULL);
    rec->state = CONTAINER_RUNNING;
    rec->stop_requested = 0;
    snprintf(rec->termination_reason, sizeof(rec->termination_reason), "%s", "running");
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    rec->nice_value = req->nice_value;
    rec->exit_code = -1;
    rec->exit_signal = 0;
    rec->log_read_fd = pipefd[0];
    rec->child_stack = stack;
    snprintf(rec->log_path, sizeof(rec->log_path), "%s/%s.log", LOG_DIR, rec->id);

    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (start_log_producer(ctx, rec) != 0) {
        (void)kill(rec->host_pid, SIGKILL);
        (void)waitpid(rec->host_pid, NULL, 0);

        pthread_mutex_lock(&ctx->metadata_lock);
        if (ctx->containers == rec)
            ctx->containers = rec->next;
        else {
            container_record_t *prev;
            for (prev = ctx->containers; prev != NULL; prev = prev->next) {
                if (prev->next == rec) {
                    prev->next = rec->next;
                    break;
                }
            }
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (rec->log_read_fd >= 0)
            close(rec->log_read_fd);
        free(rec->child_stack);
        free(rec);

        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "failed to start log producer: %s", strerror(errno));
        return -1;
    }

    if (ctx->monitor_fd >= 0) {
        if (register_with_monitor(ctx->monitor_fd,
                                  rec->id,
                                  rec->host_pid,
                                  rec->soft_limit_bytes,
                                  rec->hard_limit_bytes) < 0) {
            perror("register_with_monitor");
        }
    }

    resp->status = 0;
    snprintf(resp->message, sizeof(resp->message), "started %s pid=%d", rec->id, rec->host_pid);
    return 0;
}

static int handle_control_request(supervisor_ctx_t *ctx,
                                  const control_request_t *req,
                                  control_response_t *resp)
{
    memset(resp, 0, sizeof(*resp));

    switch (req->kind) {
    case CMD_START:
    case CMD_RUN:
        return handle_start_like_request(ctx, req, resp);
    case CMD_PS:
    {
        container_record_t *cur;
        size_t off = 0;
        int wrote_header = 0;

        pthread_mutex_lock(&ctx->metadata_lock);

        off += (size_t)snprintf(resp->message + off,
                                sizeof(resp->message) - off,
                                "%-16s %-8s %-10s %-6s %-8s %-8s %-12s\n",
                                "CONTAINER", "PID", "STATE", "NICE", "EXIT", "SIG", "REASON");
        wrote_header = 1;

        for (cur = ctx->containers; cur != NULL; cur = cur->next) {
            int n;
            const char *reason = cur->termination_reason[0] != '\0' ?
                                 cur->termination_reason : "unknown";

            n = snprintf(resp->message + off,
                         sizeof(resp->message) - off,
                         "%-16s %-8d %-10s %-6d %-8d %-8d %-12s\n",
                         cur->id,
                         cur->host_pid,
                         state_to_string(cur->state),
                         cur->nice_value,
                         cur->exit_code,
                         cur->exit_signal,
                         reason);
            if (n < 0)
                break;

            if ((size_t)n >= (sizeof(resp->message) - off)) {
                off = sizeof(resp->message) - 1;
                break;
            }

            off += (size_t)n;
        }

        if (!wrote_header)
            snprintf(resp->message, sizeof(resp->message), "CONTAINER        PID      STATE      NICE   EXIT     SIG      REASON\n(no containers)\n");
        else if (off == sizeof(resp->message) - 1 && sizeof(resp->message) > 5)
            memcpy(resp->message + sizeof(resp->message) - 5, " ...", 5);

        pthread_mutex_unlock(&ctx->metadata_lock);

        resp->status = 0;
        return 0;
    }
    case CMD_LOGS:
    {
        container_record_t *cur;

        pthread_mutex_lock(&ctx->metadata_lock);
        cur = find_container_by_id(ctx, req->container_id);
        if (cur != NULL) {
            int fd;
            ssize_t n;
            char tail[CONTROL_MESSAGE_LEN];

            resp->status = 0;
            memset(tail, 0, sizeof(tail));

            fd = open(cur->log_path, O_RDONLY);
            if (fd < 0) {
                snprintf(resp->message,
                         sizeof(resp->message),
                         "no logs yet for %s (%s)",
                         req->container_id,
                         strerror(errno));
            } else {
                off_t end = lseek(fd, 0, SEEK_END);
                off_t start = 0;

                if (end > 0) {
                    if (end > (off_t)(sizeof(tail) - 1))
                        start = end - (off_t)(sizeof(tail) - 1);
                    if (lseek(fd, start, SEEK_SET) < 0)
                        start = 0;
                }

                n = read(fd, tail, sizeof(tail) - 1);
                close(fd);

                if (n < 0) {
                    snprintf(resp->message,
                             sizeof(resp->message),
                             "failed reading logs for %s: %s",
                             req->container_id,
                             strerror(errno));
                } else if (n == 0) {
                    snprintf(resp->message,
                             sizeof(resp->message),
                             "no logs yet for %s",
                             req->container_id);
                } else {
                    tail[n] = '\0';
                    snprintf(resp->message, sizeof(resp->message), "%s", tail);
                }
            }
        } else {
            resp->status = -1;
            snprintf(resp->message, sizeof(resp->message), "unknown container: %s", req->container_id);
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        return (resp->status == 0) ? 0 : -1;
    }
    case CMD_STOP:
    {
        container_record_t *cur;

        pthread_mutex_lock(&ctx->metadata_lock);
        cur = find_container_by_id(ctx, req->container_id);
        if (cur == NULL) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            resp->status = -1;
            snprintf(resp->message, sizeof(resp->message), "unknown container: %s", req->container_id);
            return -1;
        }

        if (kill(cur->host_pid, SIGTERM) < 0) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            resp->status = -1;
            snprintf(resp->message, sizeof(resp->message), "failed to stop %s: %s", cur->id, strerror(errno));
            return -1;
        }

        cur->stop_requested = 1;
        snprintf(cur->termination_reason, sizeof(cur->termination_reason), "%s", "stop_requested");
        pthread_mutex_unlock(&ctx->metadata_lock);

        resp->status = 0;
        snprintf(resp->message, sizeof(resp->message), "stop signal sent to %s", req->container_id);
        return 0;
    }
    default:
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "unsupported command kind=%d", req->kind);
        return -1;
    }
}

static void free_container_records(supervisor_ctx_t *ctx)
{
    container_record_t *cur;

    pthread_mutex_lock(&ctx->metadata_lock);
    cur = ctx->containers;
    ctx->containers = NULL;
    pthread_mutex_unlock(&ctx->metadata_lock);

    while (cur != NULL) {
        container_record_t *next = cur->next;
        stop_log_producer(ctx, cur);
        free(cur->child_stack);
        free(cur);
        cur = next;
    }
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * TODO:
 * Implement producer-side insertion into the bounded buffer.
 *
 * Requirements:
 *   - block or fail according to your chosen policy when the buffer is full
 *   - wake consumers correctly
 *   - stop cleanly if shutdown begins
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    /* Check for shutdown; reject new items during shutdown */
    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    /* Wait while buffer is full */
    while (buffer->count >= LOG_BUFFER_CAPACITY) {
        /* Check again for shutdown to avoid race during wait */
        if (buffer->shutting_down) {
            pthread_mutex_unlock(&buffer->mutex);
            return -1;
        }
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }

    /* Insert item at head position */
    buffer->items[buffer->head] = *item;
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    /* Wake any waiting consumers */
    pthread_cond_signal(&buffer->not_empty);

    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * TODO:
 * Implement consumer-side removal from the bounded buffer.
 *
 * Requirements:
 *   - wait correctly while the buffer is empty
 *   - return a useful status when shutdown is in progress
 *   - avoid races with producers and shutdown
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    /* Wait while empty unless shutdown has begun. */
    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    /* During shutdown, an empty buffer means no more work to drain. */
    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    *item = buffer->items[buffer->tail];
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * TODO:
 * Implement the logging consumer thread.
 *
 * Suggested responsibilities:
 *   - remove log chunks from the bounded buffer
 *   - route each chunk to the correct per-container log file
 *   - exit cleanly when shutdown begins and pending work is drained
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = arg;
    log_item_t item;

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        container_record_t *cur;
        char log_path[PATH_MAX];
        size_t to_write;
        int fd;

        memset(log_path, 0, sizeof(log_path));

        pthread_mutex_lock(&ctx->metadata_lock);
        for (cur = ctx->containers; cur != NULL; cur = cur->next) {
            if (strncmp(cur->id, item.container_id, CONTAINER_ID_LEN) == 0) {
                snprintf(log_path, sizeof(log_path), "%s", cur->log_path);
                break;
            }
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (log_path[0] == '\0') {
            snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, item.container_id);
        }

        fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) {
            perror("open log file");
            continue;
        }

        to_write = item.length;
        if (to_write > LOG_CHUNK_SIZE)
            to_write = LOG_CHUNK_SIZE;

        {
            size_t written_total = 0;

        while (to_write > 0) {
            ssize_t written = write(fd,
                                    item.data + written_total,
                                    to_write);
            if (written < 0) {
                if (errno == EINTR)
                    continue;
                perror("write log file");
                break;
            }
            if (written == 0)
                break;

            written_total += (size_t)written;
            to_write -= (size_t)written;
        }
        }

        close(fd);
    }

    return NULL;
}

/*
 * TODO:
 * Implement the clone child entrypoint.
 *
 * Required outcomes:
 *   - isolated PID / UTS / mount context
 *   - chroot or pivot_root into rootfs
 *   - working /proc inside container
 *   - stdout / stderr redirected to the supervisor logging path
 *   - configured command executed inside the container
 */
int child_fn(void *arg)
{
    child_config_t *cfg = arg;
    const char *host_name;
    int devnull_fd;
    size_t host_len;

    if (cfg == NULL || cfg->rootfs[0] == '\0' || cfg->command[0] == '\0') {
        errno = EINVAL;
        perror("child_fn invalid configuration");
        return 1;
    }

    if (cfg->nice_value != 0) {
        if (setpriority(PRIO_PROCESS, 0, cfg->nice_value) < 0)
            perror("setpriority");
    }

    /* Route child output into the supervisor logging path. */
    if (cfg->log_write_fd >= 0) {
        if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0) {
            perror("dup2 stdout");
            return 1;
        }
        if (dup2(cfg->log_write_fd, STDERR_FILENO) < 0) {
            perror("dup2 stderr");
            return 1;
        }
        if (cfg->log_write_fd != STDOUT_FILENO && cfg->log_write_fd != STDERR_FILENO)
            close(cfg->log_write_fd);
    }

    devnull_fd = open("/dev/null", O_RDONLY);
    if (devnull_fd >= 0) {
        (void)dup2(devnull_fd, STDIN_FILENO);
        close(devnull_fd);
    }

    /* Keep mount changes local to this container. */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
        perror("mount MS_PRIVATE");
        return 1;
    }

    host_name = cfg->id;
    host_len = strnlen(cfg->id, CONTAINER_ID_LEN);
    if (host_len == 0) {
        host_name = "container";
        host_len = strlen(host_name);
    }
    if (sethostname(host_name, host_len) < 0) {
        perror("sethostname");
        return 1;
    }

    if (chdir(cfg->rootfs) < 0) {
        perror("chdir rootfs");
        return 1;
    }
    if (chroot(".") < 0) {
        perror("chroot");
        return 1;
    }
    if (chdir("/") < 0) {
        perror("chdir /");
        return 1;
    }

    if (mkdir("/proc", 0555) < 0 && errno != EEXIST) {
        perror("mkdir /proc");
        return 1;
    }
    if (mount("proc", "/proc", "proc", 0, NULL) < 0) {
        perror("mount /proc");
        return 1;
    }

    execl("/bin/sh", "sh", "-c", cfg->command, (char *)NULL);
    perror("exec /bin/sh");
    return 1;
}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

/*
 * TODO:
 * Implement the long-running supervisor process.
 *
 * Suggested responsibilities:
 *   - create and bind the control-plane IPC endpoint
 *   - initialize shared metadata and the bounded buffer
 *   - start the logging thread
 *   - accept control requests and update container state
 *   - reap children and respond to signals
 */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    struct sockaddr_un addr;
    int metadata_inited = 0;
    int buffer_inited = 0;
    int logger_started = 0;
    int socket_bound = 0;
    int ret = 1;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }
    metadata_inited = 1;

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        goto cleanup;
    }
    buffer_inited = 1;

    if (mkdir(LOG_DIR, 0755) < 0 && errno != EEXIST)
        perror("mkdir logs");

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        perror("open /dev/container_monitor");

    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        goto cleanup;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    unlink(CONTROL_PATH);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind control socket");
        goto cleanup;
    }
    socket_bound = 1;

    if (listen(ctx.server_fd, 16) < 0) {
        perror("listen control socket");
        goto cleanup;
    }

    if (signal(SIGINT, supervisor_term_handler) == SIG_ERR ||
        signal(SIGTERM, supervisor_term_handler) == SIG_ERR)
        perror("signal term");

    if (signal(SIGCHLD, supervisor_sigchld_handler) == SIG_ERR)
        perror("signal SIGCHLD");

    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) {
        errno = rc;
        perror("pthread_create logger_thread");
        goto cleanup;
    }
    logger_started = 1;

    fprintf(stderr, "Supervisor started (base-rootfs: %s) control=%s\n", rootfs, CONTROL_PATH);

    while (!ctx.should_stop) {
        struct pollfd pfd;
        int prc;

        if (g_stop_requested)
            ctx.should_stop = 1;

        if (g_reap_requested) {
            g_reap_requested = 0;
            reap_children(&ctx);
        }

        pfd.fd = ctx.server_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        prc = poll(&pfd, 1, 250);
        if (prc < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        if (prc > 0 && (pfd.revents & POLLIN)) {
            control_request_t req;
            control_response_t resp;
            int client_fd = accept(ctx.server_fd, NULL, NULL);

            if (client_fd < 0) {
                if (errno != EINTR)
                    perror("accept");
                continue;
            }

            if (read_full(client_fd, &req, sizeof(req)) == 0) {
                (void)handle_control_request(&ctx, &req, &resp);
                (void)write_full(client_fd, &resp, sizeof(resp));
            }

            close(client_fd);
        }
    }

    pthread_mutex_lock(&ctx.metadata_lock);
    {
        container_record_t *cur;
        for (cur = ctx.containers; cur != NULL; cur = cur->next) {
            if (cur->state == CONTAINER_RUNNING || cur->state == CONTAINER_STARTING)
                (void)kill(cur->host_pid, SIGTERM);
        }
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    reap_children(&ctx);
    stop_all_log_producers(&ctx);

    ret = 0;

cleanup:
    if (buffer_inited)
        bounded_buffer_begin_shutdown(&ctx.log_buffer);

    if (logger_started)
        pthread_join(ctx.logger_thread, NULL);

    if (ctx.server_fd >= 0)
        close(ctx.server_fd);
    if (socket_bound)
        unlink(CONTROL_PATH);
    if (ctx.monitor_fd >= 0)
        close(ctx.monitor_fd);

    if (metadata_inited)
        free_container_records(&ctx);

    if (buffer_inited)
        bounded_buffer_destroy(&ctx.log_buffer);
    if (metadata_inited)
        pthread_mutex_destroy(&ctx.metadata_lock);

    return ret;
}

/*
 * TODO:
 * Implement the client-side control request path.
 *
 * The CLI commands should use a second IPC mechanism distinct from the
 * logging pipe. A UNIX domain socket is the most direct option, but a
 * FIFO or shared memory design is also acceptable if justified.
 */
static int send_control_request(const control_request_t *req)
{
    struct sockaddr_un addr;
    control_response_t resp;
    int fd;

    if (req == NULL) {
        errno = EINVAL;
        perror("send_control_request");
        return 1;
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect supervisor");
        close(fd);
        return 1;
    }

    if (write_full(fd, req, sizeof(*req)) < 0) {
        perror("write request");
        close(fd);
        return 1;
    }

    if (read_full(fd, &resp, sizeof(resp)) < 0) {
        perror("read response");
        close(fd);
        return 1;
    }

    close(fd);

    if (resp.message[0] != '\0') {
        if (resp.status == 0)
            printf("%s\n", resp.message);
        else
            fprintf(stderr, "%s\n", resp.message);
    }

    return (resp.status == 0) ? 0 : 1;
}

static int send_control_request_raw(const control_request_t *req,
                                    control_response_t *resp_out,
                                    int print_response)
{
    struct sockaddr_un addr;
    control_response_t resp;
    int fd;

    if (req == NULL) {
        errno = EINVAL;
        perror("send_control_request");
        return 1;
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect supervisor");
        close(fd);
        return 1;
    }

    if (write_full(fd, req, sizeof(*req)) < 0) {
        perror("write request");
        close(fd);
        return 1;
    }

    if (read_full(fd, &resp, sizeof(resp)) < 0) {
        perror("read response");
        close(fd);
        return 1;
    }

    close(fd);

    if (print_response && resp.message[0] != '\0') {
        if (resp.status == 0)
            printf("%s\n", resp.message);
        else
            fprintf(stderr, "%s\n", resp.message);
    }

    if (resp_out != NULL)
        *resp_out = resp;

    return (resp.status == 0) ? 0 : 1;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    void (*old_sigint)(int);
    void (*old_sigterm)(int);
    control_request_t req;
    control_response_t resp;
    int stop_sent = 0;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    old_sigint = signal(SIGINT, run_client_term_handler);
    old_sigterm = signal(SIGTERM, run_client_term_handler);
    g_run_signal_requested = 0;

    if (send_control_request_raw(&req, &resp, 1) != 0) {
        signal(SIGINT, old_sigint);
        signal(SIGTERM, old_sigterm);
        return 1;
    }

    for (;;) {
        control_request_t ps_req;
        control_response_t ps_resp;
        char needle[CONTAINER_ID_LEN + 2];
        char *entry;
        char *state_pos;
        char *exit_pos;
        char *sig_pos;
        int exit_code = -1;
        int exit_sig = 0;
        char state_text[32];

        if (g_run_signal_requested && !stop_sent) {
            control_request_t stop_req;

            memset(&stop_req, 0, sizeof(stop_req));
            stop_req.kind = CMD_STOP;
            strncpy(stop_req.container_id, req.container_id, sizeof(stop_req.container_id) - 1);
            (void)send_control_request_raw(&stop_req, NULL, 1);

            stop_sent = 1;
            g_run_signal_requested = 0;
        }

        memset(&ps_req, 0, sizeof(ps_req));
        ps_req.kind = CMD_PS;
        if (send_control_request_raw(&ps_req, &ps_resp, 0) != 0)
            break;

        memset(needle, 0, sizeof(needle));
        snprintf(needle, sizeof(needle), "%s(", req.container_id);
        entry = strstr(ps_resp.message, needle);
        if (entry == NULL) {
            usleep(200000);
            continue;
        }

        state_pos = strstr(entry, "state=");
        exit_pos = strstr(entry, "exit=");
        sig_pos = strstr(entry, "sig=");
        if (state_pos == NULL || exit_pos == NULL || sig_pos == NULL) {
            usleep(200000);
            continue;
        }

        if (sscanf(state_pos, "state=%31[^,]", state_text) != 1 ||
            sscanf(exit_pos, "exit=%d", &exit_code) != 1 ||
            sscanf(sig_pos, "sig=%d", &exit_sig) != 1) {
            usleep(200000);
            continue;
        }

        if (strcmp(state_text, "running") == 0 || strcmp(state_text, "starting") == 0) {
            usleep(200000);
            continue;
        }

        signal(SIGINT, old_sigint);
        signal(SIGTERM, old_sigterm);

        if (exit_sig > 0)
            return 128 + exit_sig;
        if (exit_code < 0)
            return 1;
        return exit_code;
    }

    signal(SIGINT, old_sigint);
    signal(SIGTERM, old_sigterm);

    return 1;
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

    /*
     * TODO:
     * The supervisor should respond with container metadata.
     * Keep the rendering format simple enough for demos and debugging.
     */
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
