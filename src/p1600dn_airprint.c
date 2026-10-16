/*
 * p1600dn-airprint - AirPrint and IPP Everywhere for the P1600dn series:
 * an IPP/2.0 printer per CUPS queue, advertised over Bonjour, whose jobs go
 * to the local CUPS queue (and so through rastertop1600dn and its page
 * guard). Apple raster and PWG raster are converted to PDF first, since
 * macOS cannot convert them for PPD queues (src/p1600_airprint.c).
 *
 * Serving every P1600dn queue (no --queue), it follows the queue list: it
 * waits for the first queue, and when queues are added, removed or renamed
 * it stops with EXIT_RESTART so launchd starts it again with the new list.
 *
 * P1600dn serisi için AirPrint ve IPP Everywhere sunucusu.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <mach-o/dyld.h>

#include "p1600_airprint.h"
#include "p1600_queues.h"

#define INSTALLED_ICON "/Library/Printers/p1600dn/P1600dnPrinter.png"

/* How often the queue list is checked, and the exit status that asks
 * launchd (KeepAlive with SuccessfulExit false) for a restart. */
#define QUEUE_CHECK_SECONDS 60
#define EXIT_RESTART 75

static int stop_pipe[2] = {-1, -1};

static void stop_signal(int number) {
    (void)number;
    int saved = errno;
    static const char byte = 1;
    ssize_t ignored = write(stop_pipe[1], &byte, 1);
    (void)ignored;
    errno = saved;
}

static void usage(FILE *out) {
    fputs("Kullanım: p1600dn-airprint [SEÇENEKLER]\n"
          "P1600dn serisi CUPS kuyrukları için AirPrint / IPP Everywhere yazıcısı.\n"
          "  --port N            Dinlenecek port (varsayılan 8632, 0: boş bir port)\n"
          "  --listen-loopback   Yalnızca 127.0.0.1 ve ::1 üzerinde dinle (testler)\n"
          "  --no-dnssd          Bonjour ile duyurma\n"
          "  --local-only        Bonjour'u yalnızca bu Mac'te duyur (LocalOnly arayüzü)\n"
          "  --queue AD          Yalnızca bu kuyruğu sun (tekrarlanabilir)\n"
          "  --make-model METİN  printer-make-and-model (--queue ile)\n"
          "  --spool-dir DİZİN   Test modu: işleri CUPS yerine bu dizine yaz\n"
          "  --icon DOSYA        /icon.png olarak sunulacak PNG simgesi\n"
          "  -v, --verbose       Ayrıntılı günlük\n"
          "  -h, --help          Bu yardım\n"
          "\n"
          "Usage: p1600dn-airprint [OPTIONS]\n"
          "AirPrint / IPP Everywhere printer for the P1600dn series CUPS queues.\n"
          "  --port N            Port to listen on (default 8632, 0: any free port)\n"
          "  --listen-loopback   Listen on 127.0.0.1 and ::1 only (tests)\n"
          "  --no-dnssd          Do not advertise over Bonjour\n"
          "  --local-only        Advertise over Bonjour on this Mac only (LocalOnly interface)\n"
          "  --queue NAME        Serve only this queue (repeatable)\n"
          "  --make-model TEXT   printer-make-and-model (with --queue)\n"
          "  --spool-dir DIR     Test mode: write jobs to this directory instead of CUPS\n"
          "  --icon FILE         PNG icon served as /icon.png\n"
          "  -v, --verbose       Verbose log\n"
          "  -h, --help          This help\n",
          out);
}

static bool file_exists(const char *path) {
    return path && access(path, R_OK) == 0;
}

/* The icon next to the executable (the install directory), else the
 * installed one. */
static void default_icon(char *out, size_t size) {
    char executable[PATH_MAX];
    uint32_t length = sizeof(executable);
    out[0] = '\0';
    if (_NSGetExecutablePath(executable, &length) == 0) {
        char resolved[PATH_MAX];
        if (realpath(executable, resolved)) {
            char *slash = strrchr(resolved, '/');
            if (slash) {
                *slash = '\0';
                snprintf(out, size, "%s/P1600dnPrinter.png", resolved);
                if (file_exists(out)) return;
            }
        }
    }
    snprintf(out, size, "%s", file_exists(INSTALLED_ICON) ? INSTALLED_ICON : "");
}

/* The served queue list ("name\tinfo\tmake-and-model\n" each); the number
 * of queues, or -1 when CUPS could not be asked. */
static int queue_signature(char *out, size_t size) {
    P1600Queue queues[P1600_AIR_MAX_QUEUES];
    int found = p1600_queue_list(queues, P1600_AIR_MAX_QUEUES);
    out[0] = '\0';
    if (found > P1600_AIR_MAX_QUEUES) found = P1600_AIR_MAX_QUEUES;
    size_t used = 0;
    for (int i = 0; i < found && used < size; i++) {
        int written = snprintf(out + used, size - used, "%s\t%s\t%s\n", queues[i].name, queues[i].info,
                               queues[i].make_model);
        if (written < 0) break;
        used += (size_t)written;
    }
    return found;
}

/* True when a stop was requested within `seconds`. */
static bool stop_requested(int seconds) {
    struct pollfd wait = {.fd = stop_pipe[0], .events = POLLIN};
    return poll(&wait, 1, seconds * 1000) > 0;
}

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t done;
    bool finished;
    bool changed;
    char signature[8192];
} QueueWatch;

/* Checks the queue list every QUEUE_CHECK_SECONDS; on a change it marks the
 * restart and stops the server through the stop pipe. */
static void *watch_queues(void *argument) {
    QueueWatch *watch = argument;
    char current[sizeof(watch->signature)];
    pthread_mutex_lock(&watch->lock);
    while (!watch->finished) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += QUEUE_CHECK_SECONDS;
        if (pthread_cond_timedwait(&watch->done, &watch->lock, &deadline) != ETIMEDOUT) continue;
        pthread_mutex_unlock(&watch->lock);
        int found = queue_signature(current, sizeof(current));
        pthread_mutex_lock(&watch->lock);
        if (found >= 0 && !watch->finished && strcmp(current, watch->signature) != 0) {
            watch->changed = true;
            p1600_air_log("the P1600dn queues changed; restarting with the new list");
            stop_signal(SIGTERM);
            break;
        }
    }
    pthread_mutex_unlock(&watch->lock);
    return NULL;
}

static int parse_port(const char *text, int *port) {
    if (!text || !*text) return -1;
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (*end || errno || value < 0 || value > 65535) return -1;
    *port = (int)value;
    return 0;
}

int main(int argc, char **argv) {
    P1600AirConfig config;
    memset(&config, 0, sizeof(config));
    config.port = P1600_AIR_DEFAULT_PORT;
    config.dnssd = true;
    const char *queue_names[P1600_AIR_MAX_QUEUES];
    int queue_count = 0;
    const char *make_model = NULL, *icon = NULL;

    for (int i = 1; i < argc; i++) {
        const char *option = argv[i];
        if (!option) break;
        const char *value = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(option, "--port") == 0) {
            if (parse_port(value, &config.port) != 0) {
                fprintf(stderr, "airprint: geçersiz port / invalid port: %s\n", value ? value : "");
                return 2;
            }
            i++;
        } else if (strcmp(option, "--listen-loopback") == 0) {
            config.loopback = true;
        } else if (strcmp(option, "--no-dnssd") == 0) {
            config.dnssd = false;
        } else if (strcmp(option, "--local-only") == 0) {
            config.local_only = true;
        } else if (strcmp(option, "--spool-dir") == 0 && value) {
            config.spool_dir = value;
            i++;
        } else if (strcmp(option, "--queue") == 0 && value) {
            if (queue_count >= P1600_AIR_MAX_QUEUES || !*value) {
                fprintf(stderr, "airprint: çok fazla ya da boş kuyruk / too many or empty queues\n");
                return 2;
            }
            queue_names[queue_count++] = value;
            i++;
        } else if (strcmp(option, "--make-model") == 0 && value) {
            make_model = value;
            i++;
        } else if (strcmp(option, "--icon") == 0 && value) {
            icon = value;
            i++;
        } else if (strcmp(option, "-v") == 0 || strcmp(option, "--verbose") == 0) {
            config.verbose++;
        } else if (strcmp(option, "-h") == 0 || strcmp(option, "--help") == 0) {
            usage(stdout);
            return 0;
        } else {
            fprintf(stderr, "airprint: bilinmeyen seçenek / unknown option: %s\n", option);
            usage(stderr);
            return 2;
        }
    }

    if (pipe(stop_pipe) != 0) {
        perror("airprint: pipe");
        return 1;
    }
    for (int i = 0; i < 2; i++) {
        fcntl(stop_pipe[i], F_SETFD, FD_CLOEXEC);
        fcntl(stop_pipe[i], F_SETFL, fcntl(stop_pipe[i], F_GETFL) | O_NONBLOCK);
    }
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = stop_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* Queues: named ones (in test mode without CUPS), or every P1600dn queue. */
    bool test_mode = config.spool_dir && *config.spool_dir;
    QueueWatch watch = {.finished = false};
    if (queue_count > 0) {
        for (int i = 0; i < queue_count; i++) {
            P1600AirQueueSpec *spec = &config.queues[config.queue_count];
            if (test_mode) {
                snprintf(spec->name, sizeof(spec->name), "%s", queue_names[i]);
            } else {
                P1600Queue queue;
                char error[256];
                if (p1600_queue_find(queue_names[i], &queue, error, sizeof(error)) != 0) {
                    fprintf(stderr, "airprint: %s\n", error);
                    return 1;
                }
                snprintf(spec->name, sizeof(spec->name), "%s", queue.name);
                snprintf(spec->info, sizeof(spec->info), "%s", queue.info);
                snprintf(spec->make_model, sizeof(spec->make_model), "%s", queue.make_model);
            }
            if (make_model) snprintf(spec->make_model, sizeof(spec->make_model), "%s", make_model);
            config.queue_count++;
        }
        config.mirror_cups = !test_mode;
    } else {
        P1600Queue queues[P1600_AIR_MAX_QUEUES];
        int found = p1600_queue_list(queues, P1600_AIR_MAX_QUEUES);
        /* No queue yet: wait for one rather than exit (launchd would start
         * the daemon again and again). */
        for (bool told = false; found <= 0; found = p1600_queue_list(queues, P1600_AIR_MAX_QUEUES)) {
            if (!told) {
                fprintf(stderr, "airprint: P1600dn serisi CUPS kuyruğu bekleniyor / "
                                "waiting for a P1600dn series CUPS queue\n");
                told = true;
            }
            if (stop_requested(QUEUE_CHECK_SECONDS)) {
                p1600_air_log("stopped");
                return 0;
            }
        }
        queue_signature(watch.signature, sizeof(watch.signature));
        if (found > P1600_AIR_MAX_QUEUES) found = P1600_AIR_MAX_QUEUES;
        for (int i = 0; i < found; i++) {
            P1600AirQueueSpec *spec = &config.queues[config.queue_count++];
            snprintf(spec->name, sizeof(spec->name), "%s", queues[i].name);
            snprintf(spec->info, sizeof(spec->info), "%s", queues[i].info);
            snprintf(spec->make_model, sizeof(spec->make_model), "%s",
                     make_model ? make_model : queues[i].make_model);
        }
        config.mirror_cups = true;
    }

    char icon_path[PATH_MAX];
    if (icon) {
        snprintf(icon_path, sizeof(icon_path), "%s", icon);
    } else {
        default_icon(icon_path, sizeof(icon_path));
    }
    config.icon_path = icon_path;

    char error[512];
    P1600AirServer *server = p1600_air_server_create(&config, error, sizeof(error));
    if (!server) {
        fprintf(stderr, "airprint: %s\n", error);
        return 1;
    }
    pthread_t watcher;
    bool watching = !test_mode && queue_count == 0 && pthread_mutex_init(&watch.lock, NULL) == 0 &&
                    pthread_cond_init(&watch.done, NULL) == 0 &&
                    pthread_create(&watcher, NULL, watch_queues, &watch) == 0;
    int status = p1600_air_server_run(server, stop_pipe[0]);
    bool restart = false;
    if (watching) {
        pthread_mutex_lock(&watch.lock);
        watch.finished = true;
        pthread_cond_signal(&watch.done);
        pthread_mutex_unlock(&watch.lock);
        pthread_join(watcher, NULL);
        restart = watch.changed;
    }
    p1600_air_server_free(server);
    close(stop_pipe[0]);
    close(stop_pipe[1]);
    p1600_air_log("stopped");
    if (restart) return EXIT_RESTART;
    return status == 0 ? 0 : 1;
}
