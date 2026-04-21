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
#define CONTROL_MESSAGE_LEN 256
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
    CONTAINER_EXITED,
    CONTAINER_HARD_LIMIT_KILLED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    int stop_requested;
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

static supervisor_ctx_t *g_ctx = NULL;

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

static int parse_mib_flag(const char *flag, const char *value, unsigned long *target_bytes)
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

static int parse_optional_flags(control_request_t *req, int argc, char *argv[], int start_index)
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
            if (parse_mib_flag("--soft-mib", argv[i+1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i+1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i+1], &end, 10);
            if (errno != 0 || end == argv[i+1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr, "Invalid value for --nice: %s\n", argv[i+1]);
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
    case CONTAINER_STARTING:          return "starting";
    case CONTAINER_RUNNING:           return "running";
    case CONTAINER_STOPPED:           return "stopped";
    case CONTAINER_KILLED:            return "killed";
    case CONTAINER_EXITED:            return "exited";
    case CONTAINER_HARD_LIMIT_KILLED: return "hard_limit_killed";
    default:                          return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buf)
{
    int rc;
    memset(buf, 0, sizeof(*buf));
    rc = pthread_mutex_init(&buf->mutex, NULL);
    if (rc != 0) return rc;
    rc = pthread_cond_init(&buf->not_empty, NULL);
    if (rc != 0) { pthread_mutex_destroy(&buf->mutex); return rc; }
    rc = pthread_cond_init(&buf->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buf->not_empty);
        pthread_mutex_destroy(&buf->mutex);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buf)
{
    pthread_cond_destroy(&buf->not_full);
    pthread_cond_destroy(&buf->not_empty);
    pthread_mutex_destroy(&buf->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buf)
{
    pthread_mutex_lock(&buf->mutex);
    buf->shutting_down = 1;
    pthread_cond_broadcast(&buf->not_empty);
    pthread_cond_broadcast(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
}

int bounded_buffer_push(bounded_buffer_t *buf, const log_item_t *item)
{
    pthread_mutex_lock(&buf->mutex);
    while (buf->count == LOG_BUFFER_CAPACITY && !buf->shutting_down)
        pthread_cond_wait(&buf->not_full, &buf->mutex);
    if (buf->shutting_down) {
        pthread_mutex_unlock(&buf->mutex);
        return -1;
    }
    buf->items[buf->tail] = *item;
    buf->tail = (buf->tail + 1) % LOG_BUFFER_CAPACITY;
    buf->count++;
    pthread_cond_signal(&buf->not_empty);
    pthread_mutex_unlock(&buf->mutex);
    return 0;
}

int bounded_buffer_pop(bounded_buffer_t *buf, log_item_t *item)
{
    pthread_mutex_lock(&buf->mutex);
    while (buf->count == 0 && !buf->shutting_down)
        pthread_cond_wait(&buf->not_empty, &buf->mutex);
    if (buf->count == 0) {
        pthread_mutex_unlock(&buf->mutex);
        return -1;
    }
    *item = buf->items[buf->head];
    buf->head = (buf->head + 1) % LOG_BUFFER_CAPACITY;
    buf->count--;
    pthread_cond_signal(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
    return 0;
}

void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        char path[PATH_MAX];
        int fd;
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
        fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            write(fd, item.data, item.length);
            close(fd);
        }
    }
    fprintf(stderr, "[logger] consumer thread exiting, flushing done\n");
    return NULL;
}

typedef struct {
    int read_fd;
    char container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *buffer;
} producer_arg_t;

static void *producer_thread(void *arg)
{
    producer_arg_t *p = (producer_arg_t *)arg;
    log_item_t item;
    ssize_t n;
    memset(item.container_id, 0, sizeof(item.container_id));
    strncpy(item.container_id, p->container_id, CONTAINER_ID_LEN - 1);
    while ((n = read(p->read_fd, item.data, LOG_CHUNK_SIZE)) > 0) {
        item.length = (size_t)n;
        bounded_buffer_push(p->buffer, &item);
    }
    close(p->read_fd);
    free(p);
    return NULL;
}

int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;
    dup2(cfg->log_write_fd, STDOUT_FILENO);
    dup2(cfg->log_write_fd, STDERR_FILENO);
    close(cfg->log_write_fd);
    if (cfg->nice_value != 0)
        setpriority(PRIO_PROCESS, 0, cfg->nice_value);
    sethostname(cfg->id, strlen(cfg->id));
    if (chdir(cfg->rootfs) != 0) { perror("chdir"); return 1; }
    if (chroot(".") != 0) { perror("chroot"); return 1; }
    chdir("/");
    mkdir("/proc", 0555);
    mount("proc", "/proc", "proc", 0, NULL);
    execl("/bin/sh", "/bin/sh", "-c", cfg->command, NULL);
    perror("execl");
    return 1;
}

int register_with_monitor(int monitor_fd, const char *container_id,
                          pid_t host_pid, unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0) return -1;
    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0) return -1;
    return 0;
}

static container_record_t *find_container(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *c = ctx->containers;
    while (c) {
        if (strcmp(c->id, id) == 0) return c;
        c = c->next;
    }
    return NULL;
}

static void sigchld_handler(int sig)
{
    (void)sig;
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (!g_ctx) continue;
        pthread_mutex_lock(&g_ctx->metadata_lock);
        container_record_t *c = g_ctx->containers;
        while (c) {
            if (c->host_pid == pid) {
                if (WIFEXITED(status)) {
                    c->exit_code = WEXITSTATUS(status);
                    c->state = c->stop_requested ? CONTAINER_STOPPED : CONTAINER_EXITED;
                } else if (WIFSIGNALED(status)) {
                    c->exit_signal = WTERMSIG(status);
                    c->exit_code = 128 + c->exit_signal;
                    if (c->exit_signal == SIGKILL && !c->stop_requested)
                        c->state = CONTAINER_HARD_LIMIT_KILLED;
                    else
                        c->state = CONTAINER_STOPPED;
                }
                if (g_ctx->monitor_fd >= 0)
                    unregister_from_monitor(g_ctx->monitor_fd, c->id, pid);
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&g_ctx->metadata_lock);
    }
}

static void sigterm_handler(int sig)
{
    (void)sig;
    if (g_ctx) g_ctx->should_stop = 1;
}

static pid_t launch_container(supervisor_ctx_t *ctx,
                               const control_request_t *req,
                               int *pipe_read_fd_out)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;

    child_config_t *cfg = malloc(sizeof(*cfg));
    if (!cfg) { close(pipefd[0]); close(pipefd[1]); return -1; }

    strncpy(cfg->id, req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs, req->rootfs, PATH_MAX - 1);
    strncpy(cfg->command, req->command, CHILD_COMMAND_LEN - 1);
    cfg->nice_value = req->nice_value;
    cfg->log_write_fd = pipefd[1];

    char *stack = malloc(STACK_SIZE);
    if (!stack) { free(cfg); close(pipefd[0]); close(pipefd[1]); return -1; }

    pid_t pid = clone(child_fn, stack + STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD, cfg);

    close(pipefd[1]);
    free(stack);

    if (pid < 0) { free(cfg); close(pipefd[0]); return -1; }

    *pipe_read_fd_out = pipefd[0];

    producer_arg_t *pa = malloc(sizeof(*pa));
    pa->read_fd = pipefd[0];
    strncpy(pa->container_id, req->container_id, CONTAINER_ID_LEN - 1);
    pa->buffer = &ctx->log_buffer;

    pthread_t prod_tid;
    pthread_create(&prod_tid, NULL, producer_thread, pa);
    pthread_detach(prod_tid);

    return pid;
}

static void handle_request(supervisor_ctx_t *ctx, const control_request_t *req,
                            control_response_t *resp, int client_fd)
{
    memset(resp, 0, sizeof(*resp));

    switch (req->kind) {

    case CMD_START: {
        mkdir(LOG_DIR, 0755);
        int pipe_read_fd = -1;
        pid_t pid = launch_container(ctx, req, &pipe_read_fd);
        if (pid < 0) {
            resp->status = -1;
            snprintf(resp->message, CONTROL_MESSAGE_LEN, "Failed to launch container %s", req->container_id);
            break;
        }
        container_record_t *rec = calloc(1, sizeof(*rec));
        strncpy(rec->id, req->container_id, CONTAINER_ID_LEN - 1);
        rec->host_pid = pid;
        rec->started_at = time(NULL);
        rec->state = CONTAINER_RUNNING;
        rec->soft_limit_bytes = req->soft_limit_bytes;
        rec->hard_limit_bytes = req->hard_limit_bytes;
        snprintf(rec->log_path, PATH_MAX, "%s/%s.log", LOG_DIR, req->container_id);
        pthread_mutex_lock(&ctx->metadata_lock);
        rec->next = ctx->containers;
        ctx->containers = rec;
        pthread_mutex_unlock(&ctx->metadata_lock);
        if (ctx->monitor_fd >= 0)
            register_with_monitor(ctx->monitor_fd, req->container_id, pid,
                                  req->soft_limit_bytes, req->hard_limit_bytes);
        fprintf(stderr, "[supervisor] started container %s pid=%d\n", req->container_id, pid);
        resp->status = 0;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "started container %s pid=%d", req->container_id, pid);
        break;
    }

    case CMD_RUN: {
        mkdir(LOG_DIR, 0755);
        int pipe_read_fd = -1;
        pid_t pid = launch_container(ctx, req, &pipe_read_fd);
        if (pid < 0) {
            resp->status = -1;
            snprintf(resp->message, CONTROL_MESSAGE_LEN, "Failed to launch container %s", req->container_id);
            break;
        }
        container_record_t *rec = calloc(1, sizeof(*rec));
        strncpy(rec->id, req->container_id, CONTAINER_ID_LEN - 1);
        rec->host_pid = pid;
        rec->started_at = time(NULL);
        rec->state = CONTAINER_RUNNING;
        rec->soft_limit_bytes = req->soft_limit_bytes;
        rec->hard_limit_bytes = req->hard_limit_bytes;
        snprintf(rec->log_path, PATH_MAX, "%s/%s.log", LOG_DIR, req->container_id);
        pthread_mutex_lock(&ctx->metadata_lock);
        rec->next = ctx->containers;
        ctx->containers = rec;
        pthread_mutex_unlock(&ctx->metadata_lock);
        if (ctx->monitor_fd >= 0)
            register_with_monitor(ctx->monitor_fd, req->container_id, pid,
                                  req->soft_limit_bytes, req->hard_limit_bytes);
        fprintf(stderr, "[supervisor] run container %s pid=%d\n", req->container_id, pid);
        int wstatus;
        waitpid(pid, &wstatus, 0);
        int exit_code = 0;
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = find_container(ctx, req->container_id);
        if (c) {
            if (WIFEXITED(wstatus)) {
                c->exit_code = WEXITSTATUS(wstatus);
                c->state = CONTAINER_EXITED;
            } else if (WIFSIGNALED(wstatus)) {
                c->exit_signal = WTERMSIG(wstatus);
                c->exit_code = 128 + c->exit_signal;
                c->state = (c->exit_signal == SIGKILL && !c->stop_requested)
                            ? CONTAINER_HARD_LIMIT_KILLED : CONTAINER_STOPPED;
            }
            exit_code = c->exit_code;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = exit_code;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "container %s exited with code %d", req->container_id, exit_code);
        break;
    }

    case CMD_PS: {
        char buf[4096] = {0};
        int offset = 0;
        offset += snprintf(buf + offset, sizeof(buf) - offset,
                           "%-16s %-8s %-20s %-20s %-8s\n",
                           "ID", "PID", "STATE", "STARTED", "EXIT");
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        while (c && offset < (int)sizeof(buf) - 100) {
            char tstr[32];
            struct tm *tm_info = localtime(&c->started_at);
            strftime(tstr, sizeof(tstr), "%Y-%m-%d %H:%M:%S", tm_info);
            offset += snprintf(buf + offset, sizeof(buf) - offset,
                               "%-16s %-8d %-20s %-20s %-8d\n",
                               c->id, c->host_pid, state_to_string(c->state), tstr, c->exit_code);
            c = c->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = 0;
        strncpy(resp->message, buf, CONTROL_MESSAGE_LEN - 1);
        break;
    }

    case CMD_LOGS: {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, req->container_id);
        FILE *f = fopen(path, "r");
        if (!f) {
            resp->status = -1;
            snprintf(resp->message, CONTROL_MESSAGE_LEN, "No log found for %s", req->container_id);
            break;
        }
        write(client_fd, resp, sizeof(*resp));
        char lbuf[1024];
        while (fgets(lbuf, sizeof(lbuf), f))
            write(client_fd, lbuf, strlen(lbuf));
        fclose(f);
        resp->status = 0;
        memset(resp->message, 0, sizeof(resp->message));
        return;
    }

    case CMD_STOP: {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = find_container(ctx, req->container_id);
        if (!c || c->state != CONTAINER_RUNNING) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            resp->status = -1;
            snprintf(resp->message, CONTROL_MESSAGE_LEN, "Container %s not found or not running", req->container_id);
            break;
        }
        c->stop_requested = 1;
        pid_t pid = c->host_pid;
        pthread_mutex_unlock(&ctx->metadata_lock);
        kill(pid, SIGTERM);
        usleep(200000);
        kill(pid, SIGKILL);
        resp->status = 0;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "stopped container %s", req->container_id);
        break;
    }

    default:
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "Unknown command");
        break;
    }

    write(client_fd, resp, sizeof(*resp));
}

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;
    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) { perror("pthread_mutex_init"); return 1; }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) { perror("bounded_buffer_init"); return 1; }

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "[supervisor] Warning: cannot open /dev/container_monitor: %s\n", strerror(errno));

    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(ctx.server_fd, 8) < 0) { perror("listen"); return 1; }

    struct sigaction sa_chld = {0};
    sa_chld.sa_handler = sigchld_handler;
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    struct sigaction sa_term = {0};
    sa_term.sa_handler = sigterm_handler;
    sigaction(SIGINT,  &sa_term, NULL);
    sigaction(SIGTERM, &sa_term, NULL);

    pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);

    fprintf(stderr, "[supervisor] started, base-rootfs=%s\n", rootfs);

    while (!ctx.should_stop) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ctx.server_fd, &rfds);
        struct timeval tv = {1, 0};
        int sel = select(ctx.server_fd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) { if (errno == EINTR) continue; break; }
        if (sel == 0) continue;

        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) { if (errno == EINTR) continue; break; }

        control_request_t req;
        control_response_t resp;
        ssize_t n = read(client_fd, &req, sizeof(req));
        if (n == (ssize_t)sizeof(req))
            handle_request(&ctx, &req, &resp, client_fd);
        close(client_fd);
    }

    fprintf(stderr, "[supervisor] shutting down\n");

    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *c = ctx.containers;
    while (c) {
        if (c->state == CONTAINER_RUNNING) { c->stop_requested = 1; kill(c->host_pid, SIGTERM); }
        c = c->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);
    sleep(1);

    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    close(ctx.server_fd);
    unlink(CONTROL_PATH);

    pthread_mutex_lock(&ctx.metadata_lock);
    c = ctx.containers;
    while (c) { container_record_t *next = c->next; free(c); c = next; }
    pthread_mutex_unlock(&ctx.metadata_lock);

    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    fprintf(stderr, "[supervisor] exited cleanly\n");
    return 0;
}

static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect: is the supervisor running?");
        close(fd);
        return 1;
    }

    if (write(fd, req, sizeof(*req)) != (ssize_t)sizeof(*req)) { perror("write"); close(fd); return 1; }

    if (req->kind == CMD_LOGS) {
        control_response_t resp;
        read(fd, &resp, sizeof(resp));
        char buf[1024];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            fwrite(buf, 1, n, stdout);
    } else {
        control_response_t resp;
        if (read(fd, &resp, sizeof(resp)) == (ssize_t)sizeof(resp))
            printf("%s\n", resp.message);
    }

    close(fd);
    return 0;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) { fprintf(stderr, "Usage: %s start <id> <rootfs> <cmd> [...]\n", argv[0]); return 1; }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs, argv[3], PATH_MAX - 1);
    strncpy(req.command, argv[4], CHILD_COMMAND_LEN - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) { fprintf(stderr, "Usage: %s run <id> <rootfs> <cmd> [...]\n", argv[0]); return 1; }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs, argv[3], PATH_MAX - 1);
    strncpy(req.command, argv[4], CHILD_COMMAND_LEN - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) { fprintf(stderr, "Usage: %s logs <id>\n", argv[0]); return 1; }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) { fprintf(stderr, "Usage: %s stop <id>\n", argv[0]); return 1; }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }
    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]); return 1; }
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);
    usage(argv[0]);
    return 1;
}
