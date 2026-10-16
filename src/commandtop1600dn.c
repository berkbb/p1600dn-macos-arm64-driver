/*
 * commandtop1600dn - CUPS command filter for the HP LaserJet Professional
 * P1600dn series.
 *
 * Handles the PPD's `*cupsCommands`:
 *   ReportLevels       USB queues read the toner level from the printer's LEDM
 *                      document over its vendor USB interface, other queues
 *                      from Apple's snmpInk; the level goes to CUPS as
 *                      marker-* attributes.
 *   PrintSelfTestPage  The printer's Configuration Page
 *   Clean              and Cleaning Page, sent to the printer through the
 *                      queue (stdout) as PJL.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <cups/cups.h>
#include <cups/file.h>

#include "p1600_status.h"
#include "p1600_supplies.h"
#include "p1600_utility.h"

extern char **environ;

#ifndef SNMPINK_TIMEOUT_SECONDS
#define SNMPINK_TIMEOUT_SECONDS 30
#endif
#define SNMPINK_OUTPUT_LIMIT (1024U * 1024U)

/* Runs snmpInk for the queue without a shell (the queue name is passed as an
 * argument, never parsed by a shell) and returns its output, NUL-terminated. */
static char *run_snmpink(const char *queue, size_t *length) {
    int output[2];
    if (pipe(output) != 0) return NULL;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, output[0]);
    posix_spawn_file_actions_addclose(&actions, output[1]);
    char *const arguments[] = {"snmpInk", (char *)queue, NULL};
    pid_t pid = 0;
    int spawned = posix_spawn(&pid, P1600_SNMPINK_PATH, &actions, NULL, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(output[1]);
    if (spawned != 0) {
        fprintf(stderr, "DEBUG: Unable to run %s: %s\n", P1600_SNMPINK_PATH, strerror(spawned));
        close(output[0]);
        return NULL;
    }

    char *text = malloc(SNMPINK_OUTPUT_LIMIT + 1U);
    size_t used = 0;
    bool complete = false;
    time_t deadline = time(NULL) + SNMPINK_TIMEOUT_SECONDS;
    while (text) {
        int remaining = (int)(deadline - time(NULL));
        struct pollfd channel = {.fd = output[0], .events = POLLIN};
        int ready = remaining > 0 ? poll(&channel, 1, remaining * 1000) : 0;
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) break;
        ssize_t received = read(output[0], text + used, SNMPINK_OUTPUT_LIMIT - used);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) {
            complete = received == 0;
            break;
        }
        used += (size_t)received;
        if (used == SNMPINK_OUTPUT_LIMIT) break;
    }
    close(output[0]);
    if (!complete) kill(pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (!complete || !text) {
        fputs("DEBUG: snmpInk did not finish\n", stderr);
        free(text);
        return NULL;
    }
    text[used] = '\0';
    *length = used;
    return text;
}

static void report_levels(const char *queue) {
    const char *uri = getenv("DEVICE_URI");
    if (!uri || !*uri) {
        fputs("DEBUG: No device URI found in DEVICE_URI\n", stderr);
        return;
    }

    int level = -1;
    P1600UsbTarget target;
    int kind = p1600_usb_target_from_uri(uri, &target);
    if (kind < 0) {
        fputs("DEBUG: Invalid device URI\n", stderr);
        return;
    }
    if (kind == 0) {
        char error[160] = "";
        level = p1600_usb_ledm_level(&target, error, sizeof(error));
        if (level < 0) fprintf(stderr, "DEBUG: LEDM toner level unavailable: %s\n", error);
    } else {
        size_t length = 0;
        char *plist = run_snmpink(queue, &length);
        if (plist) {
            level = p1600_snmpink_level(plist, length);
            if (level < 0) fputs("DEBUG: snmpInk reported no toner level\n", stderr);
            free(plist);
        }
    }

    char line[160];
    if (level >= 0 && p1600_supply_attr_line(level, line, sizeof(line)) > 0) {
        fprintf(stderr, "DEBUG: Toner level %d%%\n", level);
        fputs(line, stderr);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 6 || argc > 7) {
        fprintf(stderr, "ERROR: Usage: %s job-id user title copies options [file]\n", argc > 0 ? argv[0] : "commandtop1600dn");
        return 1;
    }

    cups_file_t *commands = argc == 7 ? cupsFileOpen(argv[6], "r") : cupsFileStdin();
    if (!commands) {
        fprintf(stderr, "ERROR: Unable to open command file: %s\n", strerror(errno));
        return 1;
    }

    /* "#CUPS-COMMAND" and other comment lines are skipped by cupsFileGetConf. */
    char line[1024];
    char *value = NULL;
    int line_number = 0;
    while (cupsFileGetConf(commands, line, sizeof(line), &value, &line_number)) {
        char pjl[256];
        size_t length = 0;
        if (strcasecmp(line, "ReportLevels") == 0) {
            report_levels(argv[0]);
        } else if (strcasecmp(line, "PrintSelfTestPage") == 0 || strcasecmp(line, "Clean") == 0) {
            if (strcasecmp(line, "Clean") == 0) {
                length = p1600_utility_format_cleaning(pjl, sizeof(pjl));
            } else {
                /* The page in the job's language (CUPS sets LANG). */
                char language[16];
                p1600_status_language(language, sizeof(language));
                length = p1600_utility_format_info_page(P1600_INFO_CONFIGURATION, p1600_utility_pjl_language(language), pjl,
                                                     sizeof(pjl));
            }
            if (length == 0 || fwrite(pjl, 1, length, stdout) != length || fflush(stdout) != 0) {
                fprintf(stderr, "ERROR: Unable to send the %s command to the printer\n", line);
                cupsFileClose(commands);
                return 1;
            }
            fprintf(stderr, "INFO: %s\n", strcasecmp(line, "Clean") == 0 ? "Cleaning page" : "Configuration page");
        } else {
            fprintf(stderr, "DEBUG: Unknown printer command \"%s\"\n", line);
        }
    }
    cupsFileClose(commands);
    return 0;
}
