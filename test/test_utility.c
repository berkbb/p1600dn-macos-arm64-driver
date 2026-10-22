/*
 * Tests for the printer utility (src/p1600_utility.c, src/p1600_ews_proxy.c,
 * p1600dn-utility in argv[1]): the exact PJL bytes for every menu value,
 * information page languages, INQUIRE replies, a stand-in printer on the
 * loopback AppSocket port that answers like the P1606dn (one session per
 * action), the command line tool, and the EWS proxy against a stand-in
 * LEDM interface.
 *
 * Usage: test_utility <p1600dn-utility>
 */
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <cups/cups.h>

#include "p1600_ews_proxy.h"
#include "p1600_queues.h"
#include "p1600_utility.h"

#define UEL "\033%-12345X"
#define UEL_FORMAT "\033%%-12345X" /* UEL inside printf formats */

static void expect_bytes(size_t length, const char *actual, const char *expected) {
    if (length != strlen(expected) || memcmp(actual, expected, length) != 0) {
        fprintf(stderr, "got %zu bytes \"%s\", expected %zu \"%s\"\n", length, actual, strlen(expected), expected);
        assert(0);
    }
}

/* The toner level the utility shows comes from CUPS marker-levels. */
static void test_queue_levels(void) {
    assert(p1600_queue_first_level("45") == 45);
    assert(p1600_queue_first_level("0") == 0);
    assert(p1600_queue_first_level("100") == 100);
    assert(p1600_queue_first_level("70,100") == 70);
    assert(p1600_queue_first_level(NULL) == -1);
    assert(p1600_queue_first_level("") == -1);
    assert(p1600_queue_first_level("-1") == -1);
    assert(p1600_queue_first_level("-3,50") == -1);
    assert(p1600_queue_first_level("101") == -1);
    assert(p1600_queue_first_level("45%") == -1);
    assert(p1600_queue_first_level("toner") == -1);
    assert(p1600_queue_is_p1600dn("HP LaserJet Professional P1606dn, hpcups"));
    assert(!p1600_queue_is_p1600dn("HP DeskJet 3700 series"));
    /* Bonjour printers (this Mac's own AirPrint advertisement among them)
     * are not queues: printer-type 0x1000000 (CUPS_PRINTER_DISCOVERED). */
    assert(p1600_queue_is_discovered("16777220"));   /* 0x1000004: discovered, black and white */
    assert(p1600_queue_is_discovered("16777216"));
    assert(!p1600_queue_is_discovered("8396868"));    /* 0x802044: a local queue */
    assert(!p1600_queue_is_discovered("4"));
    assert(!p1600_queue_is_discovered(""));
    assert(!p1600_queue_is_discovered(NULL));
    assert(!p1600_queue_is_discovered("printer"));
    /* Driverless queues (one macOS makes from this Mac's own AirPrint
     * advertisement among them) are not driver queues. */
    assert(p1600_queue_is_p1600dn("HP LaserJet Professional P1600dn Series (Native ARM64)"));
    assert(!p1600_queue_is_p1600dn("HP LaserJet Professional P1600dn Series (Native ARM64)-AirPrint"));
    assert(!p1600_queue_is_p1600dn("HP LaserJet Professional P1600dn Series (Native ARM64)-airprint"));
    assert(!p1600_queue_is_p1600dn("HP LaserJet Professional P1606dn - IPP Everywhere"));
    assert(p1600_queue_is_driverless("HP DeskJet 3700 series-AirPrint") && !p1600_queue_is_driverless("AirPrint"));
    assert(!p1600_queue_is_driverless(NULL) && !p1600_queue_is_driverless(""));
    /* No cupsd to ask is not "no queues": -1, so the AirPrint server's queue
     * watcher keeps serving instead of restarting. */
    P1600Queue unreachable[2];
    cupsSetServer("/nonexistent/p1600dn-test-cupsd.sock");
    assert(p1600_queue_list(unreachable, 2) == -1);
    P1600Queue found;
    char error[128];
    assert(p1600_queue_find(NULL, &found, error, sizeof(error)) == -1);
    cupsSetServer(NULL);
    puts("    [PASS] Queue toner levels (marker-levels), model match, Bonjour and driverless printers left out, CUPS unreachable");
}

static void test_commands(void) {
    char out[256];
    /* Sleep after (POWERSAVETIME), 43-45 bytes. */
    static const int sleep_lengths[] = {43, 43, 44, 44, 44, 45};
    for (int i = 0; i < P1600_SLEEP_CHOICES; i++) {
        char expected[128];
        snprintf(expected, sizeof(expected), UEL_FORMAT "@PJL SET POWERSAVETIME=%d\n" UEL_FORMAT, p1600_sleep_minutes[i]);
        size_t length = p1600_utility_format_sleep(p1600_sleep_minutes[i], out, sizeof(out));
        expect_bytes(length, out, expected);
        assert((int)length == sleep_lengths[i]);
        assert(p1600_sleep_index(p1600_sleep_minutes[i]) == i && p1600_sleep_choice(i) == p1600_sleep_minutes[i]);
    }
    expect_bytes(p1600_utility_format_sleep(0, out, sizeof(out)), out, UEL "@PJL SET POWERSAVETIME=0\n" UEL);
    assert(p1600_utility_format_sleep(2, out, sizeof(out)) == 0 && p1600_utility_format_sleep(-1, out, sizeof(out)) == 0);
    assert(p1600_sleep_index(0) == -1 && p1600_sleep_index(7) == -1 && p1600_sleep_choice(6) == -1 && p1600_sleep_choice(-1) == -1);

    /* Auto Off after (INACTIVEOFFTIME, 0 = never), 45-48 bytes. */
    static const int auto_off_lengths[] = {45, 46, 46, 47, 47, 47, 48};
    for (int i = 0; i < P1600_AUTO_OFF_CHOICES; i++) {
        char expected[128];
        snprintf(expected, sizeof(expected), UEL_FORMAT "@PJL SET INACTIVEOFFTIME=%d\n" UEL_FORMAT, p1600_auto_off_minutes[i]);
        size_t length = p1600_utility_format_auto_off(p1600_auto_off_minutes[i], out, sizeof(out));
        expect_bytes(length, out, expected);
        assert((int)length == auto_off_lengths[i]);
        assert(p1600_auto_off_index(p1600_auto_off_minutes[i]) == i && p1600_auto_off_choice(i) == p1600_auto_off_minutes[i]);
    }
    assert(p1600_utility_format_auto_off(15, out, sizeof(out)) == 0 && p1600_auto_off_choice(7) == -1);

    expect_bytes(p1600_utility_format_quiet_mode(true, out, sizeof(out)), out, UEL "@PJL SET QUIETMODE=ON\n" UEL);
    expect_bytes(p1600_utility_format_quiet_mode(false, out, sizeof(out)), out, UEL "@PJL SET QUIETMODE=OFF\n" UEL);
    assert(p1600_utility_format_quiet_mode(true, out, sizeof(out)) == 40U);
    assert(p1600_utility_format_quiet_mode(false, out, sizeof(out)) == 41U);
    expect_bytes(p1600_utility_format_cleaning(out, sizeof(out)), out, UEL "@PJL CLEANPRINTER\n" UEL);
    assert(p1600_utility_format_cleaning(out, sizeof(out)) == 36U);

    expect_bytes(p1600_utility_format_info_page(P1600_INFO_DEMO, "GERMAN", out, sizeof(out)), out,
                 UEL "@PJL SET LANG=GERMAN\n@PJL SET TESTPAGE=DEMOPAGE\n" UEL);
    expect_bytes(p1600_utility_format_info_page(P1600_INFO_CONFIGURATION, "ENGLISH", out, sizeof(out)), out,
                 UEL "@PJL SET LANG=ENGLISH\n@PJL SET TESTPAGE=SELFTEST\n" UEL);
    expect_bytes(p1600_utility_format_info_page(P1600_INFO_SUPPLY_STATUS, "TURKISH", out, sizeof(out)), out,
                 UEL "@PJL SET LANG=TURKISH\n@PJL SET TESTPAGE=SUPPLIES\n" UEL);
    assert(p1600_utility_format_info_page((P1600InfoPage)3, "ENGLISH", out, sizeof(out)) == 0);
    assert(p1600_utility_format_info_page(P1600_INFO_DEMO, "영어", out, sizeof(out)) == 0);  /* translated name, not a PJL name */
    assert(p1600_utility_format_info_page(P1600_INFO_DEMO, "Language", out, sizeof(out)) == 0);  /* not a PJL name */
    assert(p1600_utility_format_info_page(P1600_INFO_DEMO, "", out, sizeof(out)) == 0);
    assert(p1600_utility_format_info_page(P1600_INFO_DEMO, NULL, out, sizeof(out)) == 0);

    expect_bytes(p1600_utility_format_inquire("POWERSAVETIME", out, sizeof(out)), out, UEL "@PJL INQUIRE POWERSAVETIME\n" UEL);
    assert(p1600_utility_format_inquire("POWERSAVETIME", out, sizeof(out)) == 45U);
    assert(p1600_utility_format_inquire("INACTIVEOFFTIME", out, sizeof(out)) == 47U);
    assert(p1600_utility_format_inquire("QUIETMODE", out, sizeof(out)) == 41U);
    assert(p1600_utility_format_inquire("quietmode", out, sizeof(out)) == 0);
    assert(p1600_utility_format_inquire("", out, sizeof(out)) == 0 && p1600_utility_format_inquire(NULL, out, sizeof(out)) == 0);
    assert(p1600_utility_format_sleep(15, out, 10) == 0 && out[0] == '\0');
    assert(p1600_utility_format_cleaning(NULL, 0) == 0);
    puts("[PASS] Utility PJL: every Sleep/Auto Off value, Quiet Mode, pages, cleaning, INQUIRE (exact bytes and lengths)");
}

static void test_languages(void) {
    static const struct {
        const char *code;
        const char *pjl;
    } cases[] = {
        {"en", "ENGLISH"}, {"en_GB", "ENGLISH"}, {"da", "DANISH"}, {"de", "GERMAN"}, {"de-CH", "GERMAN"},
        {"es", "SPANISH"}, {"fi", "FINNISH"}, {"fr", "FRENCH"}, {"it", "ITALIAN"}, {"ko", "KOREAN"},
        {"nl", "DUTCH"}, {"no", "NORWEGIAN"}, {"nb", "NORWEGIAN"}, {"nn-NO", "NORWEGIAN"}, {"pt", "PORTUGUESE"},
        {"pt-BR", "PORTUGUESE"}, {"sv", "SWEDISH"}, {"tr", "TURKISH"}, {"tr_TR", "TURKISH"}, {"ro", "ENGLISH"}, {"ro_RO", "ENGLISH"}, {"zh_CN", "CHINESE"},
        {"zh-Hans", "CHINESE"}, {"zh_TW", "TCHINESE"}, {"zh-Hant", "TCHINESE"}, {"zh_HK", "TCHINESE"},
        {"ja", "ENGLISH"}, {"tra", "ENGLISH"}, {"x", "ENGLISH"}, {"", "ENGLISH"}, {NULL, "ENGLISH"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        assert(strcmp(p1600_utility_pjl_language(cases[i].code), cases[i].pjl) == 0);
    }
    puts("[PASS] Information page languages: 14 PJL languages plus Turkish, Romanian in English, Finnish/Korean/Chinese/English by language code");
}

static void test_inquire_replies(void) {
    char value[32];
#define PARSE(text, variable) p1600_utility_parse_inquire(text, strlen(text), variable, value, sizeof(value))
    assert(PARSE("@PJL INQUIRE POWERSAVETIME\r\n15\r\n\f", "POWERSAVETIME") == 1 && strcmp(value, "15") == 0);
    assert(PARSE("@PJL INQUIRE QUIETMODE\r\nOFF\r\n\f", "QUIETMODE") == 1 && strcmp(value, "OFF") == 0);
    assert(PARSE("@PJL INQUIRE INACTIVEOFFTIME 480\n", "INACTIVEOFFTIME") == 1 && strcmp(value, "480") == 0);
    assert(PARSE("@PJL USTATUS DEVICE\r\nCODE=10001\r\n\f@PJL INQUIRE QUIETMODE\r\nON\f", "QUIETMODE") == 1 &&
           strcmp(value, "ON") == 0);
    assert(PARSE("@PJL INQUIRE QUIETMODE\r\n?\r\n\f", "QUIETMODE") == 1 && strcmp(value, "?") == 0);
    assert(PARSE("@PJL INQUIRE POWERSAVETIME\r\n1", "POWERSAVETIME") == 0);
    assert(PARSE("@PJL INQUIRE POWERSAVE", "POWERSAVETIME") == 0);
    assert(PARSE("", "QUIETMODE") == 0);
    assert(PARSE("@PJL INQUIRE QUIETMODEX\r\nON\r\n", "QUIETMODE") == -1);
    assert(PARSE("@PJL INQUIRE QUIETMODE\r\n\f", "QUIETMODE") == -1);
    assert(PARSE("@PJL INQUIRE QUIETMODE\r\n0123456789012345678901234567890123456789\r\n", "QUIETMODE") == -1);
    assert(p1600_utility_parse_inquire(NULL, 0, "QUIETMODE", value, sizeof(value)) == -1);
    assert(p1600_utility_parse_inquire("x", 1, "QUIETMODE", value, 0) == -1);
#undef PARSE
    puts("[PASS] INQUIRE replies: CR LF and LF, after other output, split, unknown (?) and malformed");
}

/* --- stand-in printer on the loopback AppSocket port ---------------------- */

typedef struct {
    int listener;
    int port;
    pthread_t thread;
    pthread_mutex_t lock;
    char received[16][256];      /* bytes of each session */
    size_t lengths[16];
    int sessions;
    const char *answers[3];      /* POWERSAVETIME, INACTIVEOFFTIME, QUIETMODE */
    atomic_bool stop;
} FakePrinter;

static void *fake_printer_main(void *argument) {
    FakePrinter *printer = argument;
    while (!atomic_load(&printer->stop)) {
        struct pollfd channel = {.fd = printer->listener, .events = POLLIN};
        if (poll(&channel, 1, 100) <= 0) continue;
        int client = accept(printer->listener, NULL, NULL);
        if (client < 0) continue;
        char data[256];
        size_t used = 0;
        /* A session is complete at its trailing UEL. */
        while (used < sizeof(data)) {
            struct pollfd wait = {.fd = client, .events = POLLIN};
            if (poll(&wait, 1, 2000) <= 0) break;
            ssize_t received = recv(client, data + used, sizeof(data) - used, 0);
            if (received <= 0) break;
            used += (size_t)received;
            if (used >= 18 && memcmp(data + used - 9, UEL, 9) == 0) break;
        }
        pthread_mutex_lock(&printer->lock);
        int session = printer->sessions < 16 ? printer->sessions++ : 15;
        memcpy(printer->received[session], data, used);
        printer->lengths[session] = used;
        pthread_mutex_unlock(&printer->lock);
        static const char *const variables[] = {"POWERSAVETIME", "INACTIVEOFFTIME", "QUIETMODE"};
        for (int v = 0; v < 3; v++) {
            char request[64];
            snprintf(request, sizeof(request), "@PJL INQUIRE %s\n", variables[v]);
            if (memmem(data, used, request, strlen(request)) && printer->answers[v]) {
                char reply[128];
                int length = snprintf(reply, sizeof(reply), "@PJL INQUIRE %s\r\n%s\r\n\f", variables[v], printer->answers[v]);
                /* In two pieces, as USB packets may come. */
                send(client, reply, 10, 0);
                struct timespec pause = {0, 20000000L};
                nanosleep(&pause, NULL);
                send(client, reply + 10, (size_t)length - 10U, 0);
            }
        }
        struct timespec linger = {0, 50000000L};
        nanosleep(&linger, NULL);
        close(client);
    }
    return NULL;
}

static void fake_printer_start(FakePrinter *printer, const char *sleep, const char *auto_off, const char *quiet) {
    memset(printer, 0, sizeof(*printer));
    atomic_init(&printer->stop, false);
    printer->answers[0] = sleep;
    printer->answers[1] = auto_off;
    printer->answers[2] = quiet;
    printer->listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t length = sizeof(address);
    assert(bind(printer->listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(printer->listener, 8) == 0);
    assert(getsockname(printer->listener, (struct sockaddr *)&address, &length) == 0);
    printer->port = ntohs(address.sin_port);
    assert(pthread_mutex_init(&printer->lock, NULL) == 0);
    assert(pthread_create(&printer->thread, NULL, fake_printer_main, printer) == 0);
}

static void fake_printer_stop(FakePrinter *printer) {
    atomic_store(&printer->stop, true);
    pthread_join(printer->thread, NULL);
    close(printer->listener);
    pthread_mutex_destroy(&printer->lock);
}

static void expect_session(FakePrinter *printer, int session, const char *expected) {
    /* A sender may close before the stand-in printer has filed the session. */
    for (int wait = 0; wait < 200; wait++) {
        pthread_mutex_lock(&printer->lock);
        bool filed = session < printer->sessions;
        pthread_mutex_unlock(&printer->lock);
        if (filed) break;
        struct timespec pause = {0, 10000000L};
        nanosleep(&pause, NULL);
    }
    pthread_mutex_lock(&printer->lock);
    assert(session < printer->sessions);
    assert(printer->lengths[session] == strlen(expected));
    assert(memcmp(printer->received[session], expected, printer->lengths[session]) == 0);
    pthread_mutex_unlock(&printer->lock);
}

static void test_link(void) {
    FakePrinter printer;
    fake_printer_start(&printer, "15", "480", "OFF");
    char uri[64], error[256] = "";
    snprintf(uri, sizeof(uri), "socket://127.0.0.1:%d", printer.port);

    P1600PrinterSettings settings;
    assert(p1600_utility_read_settings(uri, &settings, error, sizeof(error)) == 3);
    assert(settings.sleep_minutes == 15 && settings.auto_off_minutes == 480 && settings.quiet_mode == 0);
    /* One INQUIRE session per setting, in this order. */
    expect_session(&printer, 0, UEL "@PJL INQUIRE POWERSAVETIME\n" UEL);
    expect_session(&printer, 1, UEL "@PJL INQUIRE INACTIVEOFFTIME\n" UEL);
    expect_session(&printer, 2, UEL "@PJL INQUIRE QUIETMODE\n" UEL);

    char command[128];
    size_t length = p1600_utility_format_quiet_mode(true, command, sizeof(command));
    assert(p1600_utility_send(uri, command, length, error, sizeof(error)) == 0);
    expect_session(&printer, 3, UEL "@PJL SET QUIETMODE=ON\n" UEL);
    assert(p1600_utility_send(uri, command, 0, error, sizeof(error)) == -1);

    P1600PrinterLink *link = p1600_printer_link_open(uri, error, sizeof(error));
    assert(link && !p1600_printer_link_is_usb(link));
    char value[16];
    assert(p1600_printer_link_inquire(link, "quiet", value, sizeof(value)) == -1);
    p1600_printer_link_close(link);
    p1600_printer_link_close(NULL);
    fake_printer_stop(&printer);

    /* Values the menus lack, unknown variables, silence. */
    fake_printer_start(&printer, "7", "?", NULL);
    snprintf(uri, sizeof(uri), "socket://127.0.0.1:%d", printer.port);
    assert(p1600_utility_read_settings(uri, &settings, error, sizeof(error)) == 1);
    assert(settings.sleep_minutes == 7 && p1600_sleep_index(7) == -1);
    assert(settings.auto_off_minutes == -1 && settings.quiet_mode == -1);
    fake_printer_stop(&printer);

    /* A printer that takes the connection and answers nothing (busy, asleep)
     * has not been read; one that only says "?" has. */
    fake_printer_start(&printer, NULL, NULL, NULL);
    snprintf(uri, sizeof(uri), "socket://127.0.0.1:%d", printer.port);
    assert(p1600_utility_read_settings(uri, &settings, error, sizeof(error)) == -1);
    assert(strstr(error, "did not answer") != NULL);
    assert(settings.sleep_minutes == -1 && settings.auto_off_minutes == -1 && settings.quiet_mode == -1);
    fake_printer_stop(&printer);
    fake_printer_start(&printer, "?", "?", "?");
    snprintf(uri, sizeof(uri), "socket://127.0.0.1:%d", printer.port);
    assert(p1600_utility_read_settings(uri, &settings, error, sizeof(error)) == 0);
    fake_printer_stop(&printer);

    /* No printer at all. */
    assert(p1600_utility_read_settings(uri, &settings, error, sizeof(error)) == -1);
    assert(strstr(error, "127.0.0.1:") != NULL);
    assert(p1600_utility_send("file:///dev/null", command, length, error, sizeof(error)) == -1);
    assert(p1600_utility_send("serial://dev/cu.x", command, length, error, sizeof(error)) == -1);
    assert(strcmp(error, "The printer's connection cannot carry PJL") == 0);
    assert(p1600_utility_send("usb://HP/LaserJet%20Professional%20P1606dn?serial=P1600DN-TEST-NO-DEVICE", command,
                           length, error, sizeof(error)) == -1);
    assert(strcmp(error, "USB printer not found") == 0);
    assert(p1600_utility_read_settings(uri, NULL, error, sizeof(error)) == -1);

    char url[128];
    assert(p1600_utility_web_address("socket://192.0.2.7:9100", url, sizeof(url), error, sizeof(error)) == 0);
    assert(strcmp(url, "http://192.0.2.7/") == 0);
    assert(p1600_utility_web_address("ipp://[2001:db8::7]/ipp/print", url, sizeof(url), error, sizeof(error)) == 0);
    assert(strcmp(url, "http://[2001:db8::7]/") == 0);
    assert(p1600_utility_web_address("lpd://printer.local./queue", url, sizeof(url), error, sizeof(error)) == 0);
    assert(strcmp(url, "http://printer.local/") == 0);
    assert(p1600_utility_web_address("usb://HP/LaserJet%20Professional%20P1606dn?serial=X", url, sizeof(url), error,
                                  sizeof(error)) == 1);
    assert(p1600_utility_web_address("serial://dev/cu.x", url, sizeof(url), error, sizeof(error)) == -1);
    assert(p1600_utility_web_address("socket://192.0.2.7", url, 8, error, sizeof(error)) == -1);
    puts("[PASS] Printer link: INQUIRE sessions, settings, unknown values, no answer, sending, failures, web address");
}

/* --- p1600dn-utility ---------------------------------------------------- */

typedef struct {
    int status;
    char output[4096];
} Run;

static Run run_tool(const char *tool, const char *const *arguments) {
    int pipe_fds[2];
    assert(pipe(pipe_fds) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        dup2(pipe_fds[1], STDOUT_FILENO);
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        const char *argv[16] = {tool};
        int count = 1;
        for (; arguments[count - 1] && count < 15; count++) argv[count] = arguments[count - 1];
        argv[count] = NULL;
        alarm(60);
        execv(tool, (char *const *)argv);
        _exit(127);
    }
    close(pipe_fds[1]);
    Run run = {0};
    size_t used = 0;
    ssize_t received;
    while ((received = read(pipe_fds[0], run.output + used, sizeof(run.output) - 1U - used)) > 0) used += (size_t)received;
    close(pipe_fds[0]);
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    run.status = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    return run;
}

static void test_tool(const char *tool) {
    FakePrinter printer;
    fake_printer_start(&printer, "60", "0", "ON");
    char uri[64];
    snprintf(uri, sizeof(uri), "socket://127.0.0.1:%d", printer.port);

    Run run = run_tool(tool, (const char *const[]){"--en", "--device-uri", uri, "status", NULL});
    assert(run.status == 0);
    assert(strstr(run.output, "Sleep after:           60 minutes\n"));
    assert(strstr(run.output, "Auto Off after:        never\n"));
    assert(strstr(run.output, "Quiet Mode:            on\n"));
    run = run_tool(tool, (const char *const[]){"--tr", "--device-uri", uri, "status", NULL});
    assert(run.status == 0 && strstr(run.output, "Sessiz mod:            açık\n"));

    run = run_tool(tool, (const char *const[]){"--en", "--device-uri", uri, "sleep", "30", NULL});
    assert(run.status == 0 && strstr(run.output, "Sent to the printer."));
    expect_session(&printer, 6, UEL "@PJL SET POWERSAVETIME=30\n" UEL);
    run = run_tool(tool, (const char *const[]){"--device-uri", uri, "auto-off", "never", NULL});
    assert(run.status == 0 && strstr(run.output, "Yazıcıya gönderildi."));
    expect_session(&printer, 7, UEL "@PJL SET INACTIVEOFFTIME=0\n" UEL);
    run = run_tool(tool, (const char *const[]){"--device-uri", uri, "page", "configuration", "fi", NULL});
    assert(run.status == 0);
    expect_session(&printer, 8, UEL "@PJL SET LANG=FINNISH\n@PJL SET TESTPAGE=SELFTEST\n" UEL);
    run = run_tool(tool, (const char *const[]){"--en", "--device-uri", uri, "page", "supplies", NULL});
    assert(run.status == 0);
    expect_session(&printer, 9, UEL "@PJL SET LANG=ENGLISH\n@PJL SET TESTPAGE=SUPPLIES\n" UEL);
    run = run_tool(tool, (const char *const[]){"--device-uri", uri, "clean", NULL});
    assert(run.status == 0);
    expect_session(&printer, 10, UEL "@PJL CLEANPRINTER\n" UEL);
    fake_printer_stop(&printer);

    run = run_tool(tool, (const char *const[]){"--dry-run", "--device-uri", uri, "quiet", "off", NULL});
    assert(run.status == 0 && strcmp(run.output, "<ESC>%-12345X@PJL SET QUIETMODE=OFF<LF>\n<ESC>%-12345X\n") == 0);
    run = run_tool(tool, (const char *const[]){"--dry-run", "--device-uri", "socket://192.0.2.7", "web", NULL});
    assert(run.status == 0 && strcmp(run.output, "http://192.0.2.7/\n") == 0);
    run = run_tool(tool, (const char *const[]){"--dry-run", "--device-uri",
                                               "usb://HP/LaserJet%20Professional%20P1606dn?serial=X", "web", NULL});
    assert(run.status == 0 && strstr(run.output, "http://127.0.0.1:<port>/SSI/index.htm"));

    static const char *const invalid[][4] = {
        {"sleep", "2", NULL}, {"sleep", "0", NULL}, {"auto-off", "15", NULL}, {"quiet", "maybe", NULL},
        {"page", "poster", NULL}, {"page", NULL}, {"reboot", NULL},
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        const char *arguments[8] = {"--en", "--device-uri", uri};
        for (int a = 0; invalid[i][a]; a++) arguments[3 + a] = invalid[i][a];
        run = run_tool(tool, arguments);
        assert(run.status == 2 && strstr(run.output, "Usage:"));
    }
    run = run_tool(tool, (const char *const[]){"--en", "--device-uri", uri, "sleep", "30", NULL});
    assert(run.status == 1 && strstr(run.output, "Could not send to the printer: 127.0.0.1:"));
    run = run_tool(tool, (const char *const[]){"--en", "--device-uri", uri, "status", NULL});
    assert(run.status == 1 && strstr(run.output, "The printer could not be reached: "));
    run = run_tool(tool, (const char *const[]){"--en", "--queue", "P1600DN_TEST_NO_SUCH_QUEUE", "status", NULL});
    assert(run.status == 1 && strstr(run.output, "Printer queue \"P1600DN_TEST_NO_SUCH_QUEUE\" not found"));
    run = run_tool(tool, (const char *const[]){"--en", NULL});
    assert(run.status == 2);
    run = run_tool(tool, (const char *const[]){"--help", NULL});
    assert(run.status == 0 && strstr(run.output, "Kullanım:"));
    run = run_tool(tool, (const char *const[]){"--bogus", NULL});
    assert(run.status == 2);
    run = run_tool(tool, (const char *const[]){"--en", "list", NULL});
    assert(run.status == 0);
    puts("[PASS] p1600dn-utility: status (en/tr), sleep, auto-off, pages, cleaning, dry run, web, errors");
}

/* --- EWS proxy ------------------------------------------------------------- */

typedef struct {
    char request[2048];
    size_t request_length;
    const char *response;
    size_t position;
    bool fail;
    bool closed;
    int closes;
    bool late;               /* the printer answers after the proxy's read timed out */
    const char *stale;       /* that late reply, in the USB pipe in front of the next one */
    size_t reads_after_end;  /* reads after the reply ended: 10 s each over USB */
} FakeLedm;

static int ledm_write(void *context, const void *data, size_t length) {
    FakeLedm *ledm = context;
    if (ledm->fail || length > sizeof(ledm->request)) return -1;
    memcpy(ledm->request, data, length);
    ledm->request_length = length;
    ledm->position = 0;
    ledm->reads_after_end = 0;
    return 0;
}

static ssize_t ledm_read(void *context, void *buffer, size_t length, int timeout_ms) {
    (void)timeout_ms;
    FakeLedm *ledm = context;
    if (ledm->late) {
        ledm->late = false;
        ledm->stale = ledm->response;
        return -1;
    }
    bool stale = ledm->stale && *ledm->stale;
    size_t total = strlen(ledm->response);
    const char *next = stale ? ledm->stale : ledm->response + (ledm->position < total ? ledm->position : total);
    size_t left = strlen(next);
    if (left == 0) {
        ledm->reads_after_end++;
        return -1;
    }
    size_t amount = left < length ? left : length;
    if (amount > 7U) amount = 7U;  /* small packets */
    memcpy(buffer, next, amount);
    if (stale) {
        ledm->stale += amount;
    } else {
        ledm->position += amount;
    }
    return (ssize_t)amount;
}

static void ledm_close(void *context) {
    ((FakeLedm *)context)->closed = true;
    ((FakeLedm *)context)->closes++;
}

static char *browser_get(const char *url, const char *request_text) {
    unsigned port = 0;
    assert(sscanf(url, "http://127.0.0.1:%u/", &port) == 1);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(send(fd, request_text, strlen(request_text), 0) == (ssize_t)strlen(request_text));
    char *response = calloc(1, 65536);
    size_t used = 0;
    ssize_t received;
    while ((received = recv(fd, response + used, 65535U - used, 0)) > 0) used += (size_t)received;
    close(fd);
    return response;
}

static void test_ews_proxy(void) {
    FakeLedm ledm = {.response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nTransfer-Encoding: chunked\r\n"
                                 "Connection: keep-alive\r\n\r\n6\r\n<html>\r\n7\r\n</html>\r\n0\r\n\r\n"};
    P1600EwsTransport transport = {&ledm, ledm_write, ledm_read, ledm_close};
    char url[128], error[256];
    P1600EwsProxy *proxy = p1600_ews_proxy_start(transport, 30, url, sizeof(url), error, sizeof(error));
    assert(proxy && p1600_ews_proxy_running(proxy));
    assert(strncmp(url, "http://127.0.0.1:", 17) == 0 && strstr(url, "/SSI/index.htm"));
    unsigned port = 0;
    assert(sscanf(url, "http://127.0.0.1:%u/", &port) == 1);

    char get[256], post[256], request[512];
    snprintf(get, sizeof(get), "GET /SSI/index.htm HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nConnection: keep-alive\r\n\r\n",
             port);
    char *response = browser_get(url, get);
    assert(strcmp(response, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 13\r\nConnection: close\r\n\r\n"
                            "<html></html>") == 0);
    assert(ledm.request_length == strlen(get) && memcmp(ledm.request, get, ledm.request_length) == 0);
    free(response);

    /* A form post reaches the printer with its body. */
    ledm.response = "HTTP/1.1 302 Found\r\nLocation: /SSI/index.htm\r\nContent-Length: 0\r\n\r\n";
    snprintf(post, sizeof(post), "POST /SSI/set HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nOrigin: http://127.0.0.1:%u\r\n"
             "Sec-Fetch-Site: same-origin\r\nContent-Length: 11\r\n\r\nQuietMode=1", port, port);
    response = browser_get(url, post);
    assert(strstr(response, "HTTP/1.1 302 Found\r\nLocation: /SSI/index.htm\r\nContent-Length: 0\r\n"));
    assert(ledm.request_length == strlen(post) && memcmp(ledm.request, post, ledm.request_length) == 0);
    free(response);

    /* Only this proxy's own pages reach the printer: a form another site's
     * page posts, a cross-site request, a DNS rebinding page (its Host) and
     * a request without Host get 403, and nothing is sent. The browser may
     * name the proxy 127.0.0.1 or localhost. */
    static const char *const refused_heads[] = {
        "POST /SSI/set HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nOrigin: https://attacker.example\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: 11\r\n\r\nQuietMode=1",
        "POST /SSI/set HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nOrigin: null\r\nContent-Length: 11\r\n\r\nQuietMode=1",
        "POST /SSI/set HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nReferer: http://attacker.example/x.html\r\n"
        "Content-Length: 11\r\n\r\nQuietMode=1",
        "GET /SSI/index.htm HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nSec-Fetch-Site: cross-site\r\n\r\n",
        "GET /SSI/index.htm HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nSec-Fetch-Site: same-site\r\n\r\n",
        "GET /SSI/index.htm HTTP/1.1\r\nHost: rebind.attacker.example:%u\r\n\r\n",
        "GET /SSI/index.htm HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Port: %u\r\n\r\n",
        "GET /SSI/index.htm HTTP/1.1\r\nX-Port: %u\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof(refused_heads) / sizeof(refused_heads[0]); i++) {
        ledm.request_length = 0;
        snprintf(request, sizeof(request), refused_heads[i], port);
        response = browser_get(url, request);
        if (strncmp(response, "HTTP/1.1 403 Forbidden\r\n", 24) != 0 || ledm.request_length != 0) {
            fprintf(stderr, "forwarded: %s\n", request);
            assert(0 && "a request from another site reached the printer");
        }
        free(response);
    }
    snprintf(request, sizeof(request), "POST /SSI/set HTTP/1.1\r\nHost: localhost:%u\r\nOrigin: http://localhost:%u\r\n"
             "Content-Length: 11\r\n\r\nQuietMode=1", port, port);
    response = browser_get(url, request);
    assert(strncmp(response, "HTTP/1.1 302 Found\r\n", 20) == 0 && ledm.request_length == strlen(request));
    free(response);
    snprintf(request, sizeof(request), "POST /SSI/set HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n"
             "Referer: http://127.0.0.1:%u/SSI/index.htm\r\nContent-Length: 11\r\n\r\nQuietMode=1", port, port);
    response = browser_get(url, request);
    assert(strncmp(response, "HTTP/1.1 302 Found\r\n", 20) == 0 && ledm.request_length == strlen(request));
    free(response);

    /* Replies without a body end with their header (RFC 9112 section 6.3):
     * a 304 with the cached page's length, a 204, a reply to HEAD, after a
     * 100 Continue. Over USB a read past the reply waits 10 s, and a missing
     * body was a 502. */
    ledm.response = "HTTP/1.1 304 Not Modified\r\nETag: \"a1\"\r\nContent-Length: 13\r\n\r\n";
    snprintf(request, sizeof(request), "GET /SSI/index.htm HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nIf-None-Match: \"a1\"\r\n\r\n",
             port);
    response = browser_get(url, request);
    assert(strncmp(response, "HTTP/1.1 304 Not Modified\r\n", 27) == 0 && ledm.reads_after_end == 0);
    free(response);
    ledm.response = "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 204 No Content\r\n\r\n";
    response = browser_get(url, post);
    assert(strncmp(response, "HTTP/1.1 204 No Content\r\n", 25) == 0 && ledm.reads_after_end == 0);
    free(response);
    ledm.response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 13\r\n\r\n";
    snprintf(request, sizeof(request), "HEAD /SSI/index.htm HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n\r\n", port);
    response = browser_get(url, request);
    assert(strncmp(response, "HTTP/1.1 200 OK\r\n", 17) == 0 && ledm.reads_after_end == 0);
    free(response);

    /* The printer answers after the proxy gave up (502). Its late reply is
     * still in the USB pipe and must not answer the next request. */
    ledm.response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 6\r\n\r\nPage A";
    ledm.late = true;
    response = browser_get(url, get);
    assert(strncmp(response, "HTTP/1.1 502 Bad Gateway\r\n", 26) == 0);
    free(response);
    ledm.response = "HTTP/1.1 200 OK\r\nContent-Type: image/gif\r\nContent-Length: 7\r\n\r\nImage B";
    snprintf(request, sizeof(request), "GET /SSI/b.gif HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n\r\n", port);
    response = browser_get(url, request);
    assert(strstr(response, "HTTP/1.1 200 OK\r\nContent-Type: image/gif\r\n") && strstr(response, "\r\n\r\nImage B"));
    free(response);

    /* The printer does not answer. */
    ledm.fail = true;
    response = browser_get(url, get);
    assert(strncmp(response, "HTTP/1.1 502 Bad Gateway\r\n", 26) == 0);
    free(response);
    ledm.fail = false;
    ledm.response = "garbage";
    response = browser_get(url, get);
    assert(strncmp(response, "HTTP/1.1 502 Bad Gateway\r\n", 26) == 0);
    free(response);
    response = browser_get(url, "GET / HTTP/1.1\r\nNo end of headers");
    assert(response[0] == '\0');
    free(response);
    p1600_ews_proxy_stop(proxy);
    assert(ledm.closed && ledm.closes == 1);

    /* The proxy ends after its idle time. */
    FakeLedm idle = {.response = ""};
    P1600EwsTransport idle_transport = {&idle, ledm_write, ledm_read, ledm_close};
    proxy = p1600_ews_proxy_start(idle_transport, 1, url, sizeof(url), error, sizeof(error));
    assert(proxy);
    p1600_ews_proxy_wait(proxy);
    assert(!p1600_ews_proxy_running(proxy));
    /* Idle, it lets go of the printer's interface at once (the toner query
     * needs it while the utility stays open), and its address refuses
     * connections; stopping it later closes nothing twice. */
    assert(idle.closed && idle.closes == 1);
    unsigned idle_port = 0;
    assert(sscanf(url, "http://127.0.0.1:%u/", &idle_port) == 1);
    int refused = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in idle_address = {.sin_family = AF_INET, .sin_port = htons((uint16_t)idle_port),
                                       .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(refused >= 0 && connect(refused, (struct sockaddr *)&idle_address, sizeof(idle_address)) != 0 &&
           errno == ECONNREFUSED);
    close(refused);
    p1600_ews_proxy_stop(proxy);
    assert(idle.closes == 1);

    P1600EwsTransport broken = {NULL, NULL, NULL, NULL};
    assert(!p1600_ews_proxy_start(broken, 1, url, sizeof(url), error, sizeof(error)));
    assert(!p1600_ews_proxy_start_usb("socket://192.0.2.7", 1, url, sizeof(url), error, sizeof(error)));
    assert(strcmp(error, "Not a USB printer") == 0);
    assert(!p1600_ews_proxy_start_usb("usb://HP/LaserJet%20Professional%20P1606dn?serial=P1600DN-TEST-NO-DEVICE", 1,
                                   url, sizeof(url), error, sizeof(error)));
    assert(!p1600_ews_proxy_running(NULL));
    p1600_ews_proxy_wait(NULL);
    p1600_ews_proxy_stop(NULL);
    puts("[PASS] EWS proxy: chunked pages re-framed, form posts, printer failures (502), idle end frees the printer");
    puts("[PASS] EWS proxy: other sites' requests refused (Host, Origin, Referer, Sec-Fetch-Site), bodiless replies, "
         "late replies dropped");
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <p1600dn-utility>\n", argv[0]);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    test_queue_levels();
    test_commands();
    test_languages();
    test_inquire_replies();
    test_link();
    test_tool(argv[1]);
    test_ews_proxy();
    puts("[PASS] Printer utility suite");
    return 0;
}
