/*
 * Tests for the toner level support (src/p1600_supplies.c, src/p1600_usb_ledm.c)
 * and the commandtop1600dn command filter (argv[1], built against
 * test/fake_snmpink.sh instead of Apple's snmpInk): usb:// URIs, USB device
 * matching, HTTP responses, the LEDM document, snmpInk plists, the ATTR line,
 * and ReportLevels command jobs.
 *
 * Usage: test_supplies <commandtop1600dn test build>
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "p1600_supplies.h"

static void test_uri(void) {
    P1600UsbTarget target;
    assert(p1600_usb_target_from_uri("usb://HP/LaserJet%20Professional%20P1606dn?serial=000000000Q81TEST&interface=1",
                                  &target) == 0);
    assert(strcmp(target.make, "HP") == 0);
    assert(strcmp(target.product, "LaserJet Professional P1606dn") == 0);
    assert(strcmp(target.serial, "000000000Q81TEST") == 0);

    assert(p1600_usb_target_from_uri("usb://HP/LaserJet%20Professional%20P1606dn", &target) == 0);
    assert(strcmp(target.product, "LaserJet Professional P1606dn") == 0 && target.serial[0] == '\0');
    assert(p1600_usb_target_from_uri("usb://HP/LaserJet%20P1606dn?interface=1&serial=ABC", &target) == 0);
    assert(strcmp(target.serial, "ABC") == 0);
    assert(p1600_usb_target_from_uri("usb://HP/?serial=ABC", &target) == 0);
    assert(target.product[0] == '\0' && strcmp(target.serial, "ABC") == 0);
    assert(p1600_usb_target_from_uri("usb://HP/", &target) == -1);
    assert(p1600_usb_target_from_uri("usb://HP/?interface=1", &target) == -1);
    assert(p1600_usb_target_from_uri("socket://192.0.2.10:9100", &target) == 1);
    assert(p1600_usb_target_from_uri("dnssd://HP%20LaserJet._pdl-datastream._tcp.local./?uuid=1", &target) == 1);
    assert(p1600_usb_target_from_uri("not a uri", &target) == -1);
    assert(p1600_usb_target_from_uri(NULL, &target) == -1);
    assert(p1600_usb_target_from_uri("usb://HP/P1606dn", NULL) == -1);
    puts("[PASS] usb:// URI: make, product and serial parameter");
}

static void test_device_match(void) {
    P1600UsbTarget target;
    assert(p1600_usb_target_from_uri("usb://HP/LaserJet%20Professional%20P1606dn?serial=SN1", &target) == 0);
    assert(p1600_usb_device_matches(&target, "LaserJet Professional P1606dn", "SN1", 0));
    assert(p1600_usb_device_matches(&target, "HP LaserJet Professional P1606dn", "SN1", 0));
    assert(p1600_usb_device_matches(&target, "hp laserjet professional p1606dn", "SN1", 0));
    assert(p1600_usb_device_matches(&target, "Other Name", "SN1", P1600_USB_VENDOR_ID));
    assert(p1600_usb_device_matches(&target, NULL, "SN1", P1600_USB_VENDOR_ID));
    assert(!p1600_usb_device_matches(&target, "Other Name", "SN1", 0x05AC));
    assert(!p1600_usb_device_matches(&target, "HP LaserJet Professional P1606dn", "SN2", P1600_USB_VENDOR_ID));
    assert(!p1600_usb_device_matches(&target, "HP LaserJet Professional P1606dn", "sn1", P1600_USB_VENDOR_ID));
    assert(!p1600_usb_device_matches(&target, "HP LaserJet Professional P1606dn", NULL, P1600_USB_VENDOR_ID));
    assert(!p1600_usb_device_matches(&target, "HPX LaserJet Professional P1606dn", "SN1", 0));
    assert(!p1600_usb_device_matches(&target, "HP  LaserJet Professional P1606dn", "SN1", 0));

    assert(p1600_usb_target_from_uri("usb://HP/LaserJet%20Professional%20P1606dn", &target) == 0);
    assert(p1600_usb_device_matches(&target, "HP LaserJet Professional P1606dn", "anything", 0));
    assert(p1600_usb_device_matches(&target, "LaserJet Professional P1606dn", NULL, 0));
    assert(!p1600_usb_device_matches(&target, "HP LaserJet Professional P1566", "anything", P1600_USB_VENDOR_ID));
    assert(!p1600_usb_device_matches(&target, NULL, NULL, P1600_USB_VENDOR_ID));
    assert(!p1600_usb_device_matches(NULL, "HP LaserJet Professional P1606dn", "SN1", P1600_USB_VENDOR_ID));

    memset(&target, 0, sizeof(target));
    strcpy(target.product, "P1606dn");
    assert(p1600_usb_device_matches(&target, "P1606dn", NULL, 0));
    assert(!p1600_usb_device_matches(&target, " P1606dn", NULL, 0));
    puts("[PASS] USB device match: serial, make prefix, letter case, HP vendor id");
}

typedef struct {
    const char *data;
    size_t length;
    size_t position;
    size_t step;        /* bytes per read */
    size_t fail_at;     /* -1 at this offset (SIZE_MAX: never) */
} ScriptedReader;

static ssize_t scripted_read(void *context, unsigned char *buffer, size_t length) {
    ScriptedReader *reader = context;
    if (reader->position >= reader->fail_at) return -1;
    if (reader->position >= reader->length) return 0;
    size_t amount = reader->length - reader->position;
    if (amount > reader->step) amount = reader->step;
    if (amount > length) amount = length;
    if (reader->fail_at != SIZE_MAX && reader->position + amount > reader->fail_at) amount = reader->fail_at - reader->position;
    memcpy(buffer, reader->data + reader->position, amount);
    reader->position += amount;
    return (ssize_t)amount;
}

static int http(const char *response, size_t step, size_t fail_at, size_t capacity, char *body, size_t *length) {
    ScriptedReader reader = {response, strlen(response), 0, step, fail_at};
    return p1600_http_read_response(scripted_read, &reader, (unsigned char *)body, capacity, length);
}

static void expect_body(const char *response, int status, const char *expected) {
    static const size_t steps[] = {1, 2, 3, 7, 64, 4096, 100000};
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        char body[512];
        size_t length = 99;
        int result = http(response, steps[i], SIZE_MAX, sizeof(body), body, &length);
        if (result != status) {
            fprintf(stderr, "step %zu: expected status %d, got %d for:\n%s\n", steps[i], status, result, response);
            assert(0);
        }
        if (status >= 0) {
            assert(length == strlen(expected));
            assert(memcmp(body, expected, length) == 0);
        } else {
            assert(length == 0);
        }
    }
}

static void test_http(void) {
    expect_body("HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\nContent-Length: 11\r\n\r\n<a>body</a>", 200, "<a>body</a>");
    expect_body("HTTP/1.1 200 OK\r\ncontent-length:  5\r\n\r\nhello", 200, "hello");
    expect_body("HTTP/1.0 200 OK\nContent-Length: 2\n\nok", 200, "ok");
    expect_body("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\n<dd:\r\n6;ext=1\r\nLevel>\r\n0\r\n\r\n", 200,
                "<dd:Level>");
    expect_body("HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked\r\n\r\na\r\n0123456789\r\n0\r\nX-Trailer: 1\r\n\r\n", 200,
                "0123456789");
    expect_body("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n", 200, "abc");
    expect_body("HTTP/1.1 200 OK\r\nServer: HP\r\n\r\nuntil the end", 200, "until the end");
    expect_body("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 200, "");
    expect_body("HTTP/1.1 404 Not Found\r\nContent-Length: 3\r\n\r\nno!", 404, "no!");
    expect_body("HTTP/1.1 200 OK\r\nBad header line\r\nContent-Length: 2\r\n\r\nok", 200, "ok");

    expect_body("", -1, NULL);
    expect_body("HTTP/1.1\r\n\r\n", -1, NULL);
    expect_body("HTTP/1.1 99 Too Low\r\n\r\n", -1, NULL);
    expect_body("ICY 200 OK\r\n\r\n", -1, NULL);
    expect_body("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort", -1, NULL);
    expect_body("HTTP/1.1 200 OK\r\nContent-Length: x\r\n\r\n", -1, NULL);
    expect_body("HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n", -1, NULL);
    expect_body("HTTP/1.1 200 OK\r\nContent-Length: 2", -1, NULL);
    expect_body("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n", -1, NULL);
    expect_body("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabcX\r\n0\r\n\r\n", -1, NULL);
    expect_body("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nab", -1, NULL);
    expect_body("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n", -1, NULL);
    expect_body("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nFFFFFFFFFFFFFFFFFFFF\r\n", -1, NULL);

    /* Limits: body capacity, header line length, header count. */
    char body[8];
    size_t length = 0;
    assert(http("HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\n123456789", 64, SIZE_MAX, sizeof(body), body, &length) == -1);
    assert(http("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n9\r\n123456789\r\n0\r\n\r\n", 64, SIZE_MAX,
                sizeof(body), body, &length) == -1);
    assert(http("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\n12345\r\n5\r\n67890\r\n0\r\n\r\n", 64, SIZE_MAX,
                sizeof(body), body, &length) == -1);
    assert(http("HTTP/1.1 200 OK\r\n\r\n123456789", 3, SIZE_MAX, sizeof(body), body, &length) == -1);
    assert(http("HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\n12345678", 64, SIZE_MAX, sizeof(body), body, &length) == 200);
    assert(length == 8 && memcmp(body, "12345678", 8) == 0);

    char *long_header = malloc(3000);
    assert(long_header != NULL);
    strcpy(long_header, "HTTP/1.1 200 OK\r\nX-Long: ");
    size_t prefix = strlen(long_header);
    memset(long_header + prefix, 'x', 1500);
    strcpy(long_header + prefix + 1500, "\r\n\r\n");
    assert(http(long_header, 64, SIZE_MAX, sizeof(body), body, &length) == -1);
    free(long_header);

    char many[4096] = "HTTP/1.1 200 OK\r\n";
    for (int i = 0; i < 120; i++) strcat(many, "X-A: b\r\n");
    strcat(many, "\r\n");
    assert(http(many, 4096, SIZE_MAX, sizeof(body), body, &length) == -1);

    /* A reader timeout (-1) ends a stream like the end of data. */
    static const char timed[] = "HTTP/1.1 200 OK\r\nServer: HP\r\n\r\nabcdef";
    assert(http(timed, 4, sizeof(timed) - 3U, sizeof(body), body, &length) == 200);
    assert(length == 4 && memcmp(body, "abcd", 4) == 0);
    assert(http(timed, 4, 10, sizeof(body), body, &length) == -1);

    assert(p1600_http_read_response(NULL, NULL, (unsigned char *)body, sizeof(body), &length) == -1);
    ScriptedReader reader = {"HTTP/1.1 200 OK\r\n\r\n", 19, 0, 64, SIZE_MAX};
    assert(p1600_http_read_response(scripted_read, &reader, (unsigned char *)body, sizeof(body), NULL) == -1);
    assert(p1600_http_read_response(scripted_read, &reader, NULL, 4, &length) == -1);
    reader.position = 0;
    assert(p1600_http_read_response(scripted_read, &reader, NULL, 0, &length) == 200 && length == 0);
    puts("[PASS] HTTP responses: Content-Length, chunked, read to end, split reads, malformed input, limits");
}

static int ledm(const char *xml) {
    return p1600_ledm_level(xml, strlen(xml));
}

static int snmp(const char *text) {
    return p1600_snmpink_level(text, strlen(text));
}

static void test_ledm(void) {
    static const char document[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<ccdyn:ConsumableConfigDyn xmlns:dd=\"http://www.hp.com/schemas/imaging/con/dictionaries/1.0/\">\n"
        "  <ccdyn:ConsumableInfo>\n"
        "    <dd:ConsumableLabelCode>K</dd:ConsumableLabelCode>\n"
        "    <dd:ConsumablePercentageLevelRemaining>63</dd:ConsumablePercentageLevelRemaining>\n"
        "  </ccdyn:ConsumableInfo>\n"
        "  <ccdyn:ConsumableInfo>\n"
        "    <dd:ConsumablePercentageLevelRemaining>12</dd:ConsumablePercentageLevelRemaining>\n"
        "  </ccdyn:ConsumableInfo>\n"
        "</ccdyn:ConsumableConfigDyn>\n";
    assert(p1600_ledm_level(document, sizeof(document) - 1U) == 63);
    assert(ledm("<dd:ConsumablePercentageLevelRemaining> 0 </dd:ConsumablePercentageLevelRemaining>") == 0);
    assert(ledm("<dd:ConsumablePercentageLevelRemaining>100</dd:") == 100);
    assert(ledm("<dd:ConsumablePercentageLevelRemaining>101</dd:") == -1);
    assert(ledm("<dd:ConsumablePercentageLevelRemaining>-5</dd:") == -1);
    assert(ledm("<dd:ConsumablePercentageLevelRemaining>x</dd:") == -1);
    assert(ledm("<dd:ConsumablePercentageLevelRemaining>45") == -1);
    assert(ledm("<dd:ConsumablePercentageLevelRemaining>99999999999</dd:") == -1);
    assert(ledm("<ConsumablePercentageLevelRemaining>45</ConsumablePercentageLevelRemaining>") == -1);
    /* Only `length` bytes are examined (the body is not NUL-terminated). */
    assert(p1600_ledm_level(document, 200) == -1);
    assert(p1600_ledm_level(NULL, 10) == -1);
    puts("[PASS] LEDM document: first ConsumablePercentageLevelRemaining, range, malformed values");
}

static char *plist(const char *level, const char *capacity) {
    char *text = NULL;
    assert(asprintf(&text,
                    "<plist version=\"1.0\">\n<dict>\n\t<key>prtMarkerSuppliesTable</key>\n\t<array>\n\t\t<dict>\n"
                    "\t\t\t<key>prtMarkerSuppliesLevel</key>\n\t\t\t<integer>%s</integer>\n"
                    "%s%s%s"
                    "\t\t</dict>\n\t\t<dict>\n"
                    "\t\t\t<key>prtMarkerSuppliesLevel</key>\n\t\t\t<integer>7</integer>\n"
                    "\t\t\t<key>prtMarkerSuppliesMaxCapacity</key>\n\t\t\t<integer>10</integer>\n"
                    "\t\t</dict>\n\t</array>\n</dict>\n</plist>\n",
                    level, capacity ? "\t\t\t<key>prtMarkerSuppliesMaxCapacity</key>\n\t\t\t<integer>" : "",
                    capacity ? capacity : "", capacity ? "</integer>\n" : "") > 0);
    return text;
}

static int plist_level(const char *level, const char *capacity) {
    char *text = plist(level, capacity);
    int result = p1600_snmpink_level(text, strlen(text));
    free(text);
    return result;
}

static void test_snmpink(void) {
    assert(plist_level("45", "100") == 45);
    assert(plist_level("300", "1200") == 25);
    assert(plist_level("1200", "1200") == 100);
    assert(plist_level("1500", "1200") == 100);
    assert(plist_level("0", "1200") == 0);
    assert(plist_level("80", "-2") == 80);
    assert(plist_level("150", "-2") == -1);
    assert(plist_level("80", "0") == 80);
    assert(plist_level("80", NULL) == 80);   /* the second supply's capacity is not used */
    assert(plist_level("-3", "100") == -1);
    assert(plist_level("-2", "100") == -1);
    assert(plist_level("x", "100") == -1);
    assert(plist_level("", "100") == -1);
    assert(plist_level("123456789012", "100") == -1);
    assert(plist_level("50", "x") == 50);
    assert(snmp("<key>prtMarkerSuppliesLevel</key><string>45</string>") == -1);
    assert(snmp("<key>prtMarkerSuppliesLevel</key>\n<integer>45") == -1);
    assert(snmp("<key>prtMarkerSuppliesLevel</key>") == -1);
    assert(snmp("<plist><dict></dict></plist>") == -1);
    assert(p1600_snmpink_level(NULL, 4) == -1);
    puts("[PASS] snmpInk plist: first supply, MaxCapacity scaling, unknown capacity, negative levels");
}

static void test_attr_line(void) {
    char line[128];
    assert(p1600_supply_attr_line(0, line, sizeof(line)) > 0);
    assert(strcmp(line, "ATTR: marker-colors=#000000 marker-levels=0 marker-names=Black marker-types=toner\n") == 0);
    assert(p1600_supply_attr_line(100, line, sizeof(line)) > 0);
    assert(strcmp(line, "ATTR: marker-colors=#000000 marker-levels=100 marker-names=Black marker-types=toner\n") == 0);
    assert(p1600_supply_attr_line(-1, line, sizeof(line)) == 0 && line[0] == '\0');
    assert(p1600_supply_attr_line(101, line, sizeof(line)) == 0 && line[0] == '\0');
    assert(p1600_supply_attr_line(50, line, 20) == 0 && line[0] == '\0');
    assert(p1600_supply_attr_line(50, NULL, 20) == 0);
    assert(p1600_supply_attr_line(50, line, 0) == 0);
    assert(strlen(P1600_LEDM_REQUEST) == 49U);
    puts("[PASS] ATTR marker line and the 49-byte LEDM request");
}

static void test_usb_lookup(void) {
    /* Enumerates this Mac's USB devices; no printer has this serial. */
    P1600UsbTarget target;
    assert(p1600_usb_target_from_uri("usb://HP/LaserJet%20Professional%20P1606dn?serial=P1600DN-TEST-NO-DEVICE",
                                  &target) == 0);
    char error[160] = "";
    assert(p1600_usb_ledm_level(&target, error, sizeof(error)) == -1);
    assert(strcmp(error, "USB printer not found") == 0);
    assert(p1600_usb_ledm_level(NULL, error, sizeof(error)) == -1);
    assert(p1600_usb_ledm_level(&target, NULL, 0) == -1);
    puts("[PASS] USB LEDM lookup without a matching printer");
}

/* --- commandtop1600dn --------------------------------------------------- */

typedef struct {
    int status;
    char *diagnostics;
    char *output;
    size_t output_length;
    double seconds;
} CommandRun;

static const char *command_language = "tr_TR.UTF-8";

static double now_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static char *read_all(int fd, size_t *length) {
    size_t capacity = 4096, used = 0;
    char *data = malloc(capacity + 1U);
    assert(data != NULL);
    for (;;) {
        if (used == capacity) {
            capacity *= 2U;
            data = realloc(data, capacity + 1U);
            assert(data != NULL);
        }
        ssize_t received = read(fd, data + used, capacity - used);
        assert(received >= 0);
        if (received == 0) break;
        used += (size_t)received;
    }
    data[used] = '\0';
    if (length) *length = used;
    return data;
}

/* Runs the filter as CUPS does (argv[0] = queue name). `commands` goes to
 * stdin when `file` is NULL. */
static CommandRun run_command(const char *filter, const char *queue, const char *device_uri,
                              const char *commands, const char *file, int argc) {
    int input[2], output[2], errors[2];
    assert(pipe(input) == 0 && pipe(output) == 0 && pipe(errors) == 0);
    double start = now_seconds();
    fflush(stdout);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        if (dup2(input[0], STDIN_FILENO) < 0 || dup2(output[1], STDOUT_FILENO) < 0 ||
            dup2(errors[1], STDERR_FILENO) < 0) _exit(126);
        for (int fd = 3; fd < 64; fd++) close(fd);
        if (device_uri) setenv("DEVICE_URI", device_uri, 1);
        else unsetenv("DEVICE_URI");
        setenv("LANG", command_language, 1);
        unsetenv("LC_ALL");
        unsetenv("LC_MESSAGES");
        alarm(60);
        if (argc == 7) execl(filter, queue, "1", "user", "ReportLevels", "1", "", file, (char *)NULL);
        else if (argc == 6) execl(filter, queue, "1", "user", "ReportLevels", "1", "", (char *)NULL);
        else execl(filter, queue, "1", (char *)NULL);
        _exit(127);
    }
    close(input[0]);
    close(output[1]);
    close(errors[1]);
    if (commands) {
        ssize_t written = write(input[1], commands, strlen(commands));
        assert(written == (ssize_t)strlen(commands) || (written < 0 && errno == EPIPE));
    }
    close(input[1]);

    CommandRun run = {0};
    run.output = read_all(output[0], &run.output_length);
    run.diagnostics = read_all(errors[0], NULL);
    close(output[0]);
    close(errors[0]);
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    run.seconds = now_seconds() - start;
    run.status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return run;
}

static void expect_run(CommandRun run, int status, const char *present, const char *absent) {
    if (run.status != status || (present && !strstr(run.diagnostics, present)) ||
        (absent && strstr(run.diagnostics, absent))) {
        fprintf(stderr, "status %d (expected %d), diagnostics:\n%s\n", run.status, status, run.diagnostics);
        assert(0);
    }
    assert(run.output_length == 0);
    free(run.diagnostics);
    free(run.output);
}

/* The printer's Configuration and Cleaning pages as the CUPS commands
 * PrintSelfTestPage and Clean, written to the printer as PJL. */
static void test_maintenance_commands(const char *filter) {
    static const char network[] = "socket://192.0.2.10:9100";
    command_language = "de_DE.UTF-8";
    CommandRun run = run_command(filter, "levels-45", network, "#CUPS-COMMAND\nPrintSelfTestPage\nClean\n", NULL, 6);
    static const char expected[] = "\033%-12345X@PJL SET LANG=GERMAN\n@PJL SET TESTPAGE=SELFTEST\n\033%-12345X"
                                   "\033%-12345X@PJL CLEANPRINTER\n\033%-12345X";
    assert(run.status == 0);
    assert(run.output_length == sizeof(expected) - 1U && memcmp(run.output, expected, run.output_length) == 0);
    assert(strstr(run.diagnostics, "INFO: Configuration page\n") && strstr(run.diagnostics, "INFO: Cleaning page\n"));
    free(run.output);
    free(run.diagnostics);

    command_language = "C";
    run = run_command(filter, "levels-45", network, "#CUPS-COMMAND\nprintselftestpage\n", NULL, 6);
    static const char english[] = "\033%-12345X@PJL SET LANG=ENGLISH\n@PJL SET TESTPAGE=SELFTEST\n\033%-12345X";
    assert(run.status == 0 && run.output_length == sizeof(english) - 1U && memcmp(run.output, english, run.output_length) == 0);
    free(run.output);
    free(run.diagnostics);
    command_language = "tr_TR.UTF-8";
    puts("[PASS] commandtop1600dn: PrintSelfTestPage and Clean send their PJL in the job's language");
}

static void test_command_filter(const char *filter) {
    static const char report[] = "#CUPS-COMMAND\nReportLevels\n";
    static const char *const network = "socket://192.0.2.10:9100";

    expect_run(run_command(filter, "levels-45", network, report, NULL, 6), 0,
               "ATTR: marker-colors=#000000 marker-levels=45 marker-names=Black marker-types=toner\n", NULL);
    expect_run(run_command(filter, "scaled", network, report, NULL, 6), 0, "marker-levels=25 ", NULL);
    expect_run(run_command(filter, "unknown-capacity", "dnssd://HP._pdl-datastream._tcp.local./", report, NULL, 6), 0,
               "marker-levels=80 ", NULL);
    expect_run(run_command(filter, "some-remaining", network, report, NULL, 6), 0,
               "DEBUG: snmpInk reported no toner level\n", "ATTR:");
    expect_run(run_command(filter, "silent", network, report, NULL, 6), 0, "DEBUG: snmpInk reported no toner level\n",
               "ATTR:");
    expect_run(run_command(filter, "failing", network, report, NULL, 6), 0, NULL, "ATTR:");

    CommandRun slow = run_command(filter, "slow", network, report, NULL, 6);
    assert(slow.seconds < 8.0);
    expect_run(slow, 0, "DEBUG: snmpInk did not finish\n", "ATTR:");

    /* Every ReportLevels line is answered in any letter case; comment lines are skipped. */
    CommandRun twice = run_command(filter, "levels-45", network,
                                   "#CUPS-COMMAND\n# comment\nreportlevels\nPrintAlignmentPage\nReportLevels extra\n", NULL, 6);
    char *first = strstr(twice.diagnostics, "marker-levels=45");
    assert(first && strstr(first + 1, "marker-levels=45"));
    expect_run(twice, 0, "DEBUG: Unknown printer command \"PrintAlignmentPage\"\n", NULL);

    const char *directory = getenv("TMPDIR");
    char path[1024];
    snprintf(path, sizeof(path), "%s/p1600dn-command-XXXXXX", directory && *directory ? directory : "/tmp");
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(write(fd, report, sizeof(report) - 1U) == (ssize_t)(sizeof(report) - 1U));
    close(fd);
    expect_run(run_command(filter, "levels-45", network, NULL, path, 7), 0, "marker-levels=45 ", NULL);
    unlink(path);

    expect_run(run_command(filter, "levels-45", network, NULL, "/nonexistent/command-file", 7), 1,
               "ERROR: Unable to open command file:", NULL);
    expect_run(run_command(filter, "levels-45", network, report, NULL, 2), 1, "ERROR: Usage:", NULL);
    expect_run(run_command(filter, "levels-45", NULL, report, NULL, 6), 0, "DEBUG: No device URI found in DEVICE_URI\n",
               "ATTR:");
    expect_run(run_command(filter, "levels-45", "usb://HP/", report, NULL, 6), 0, "DEBUG: Invalid device URI\n", "ATTR:");
    expect_run(run_command(filter, "levels-45",
                           "usb://HP/LaserJet%20Professional%20P1606dn?serial=P1600DN-TEST-NO-DEVICE", report, NULL, 6),
               0, "DEBUG: LEDM toner level unavailable: USB printer not found\n", "ATTR:");
    expect_run(run_command(filter, "levels-45", network, "", NULL, 6), 0, NULL, "ATTR:");
    puts("[PASS] commandtop1600dn: ReportLevels via snmpInk and USB, command files, errors");
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <commandtop1600dn test build>\n", argv[0]);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    test_uri();
    test_device_match();
    test_http();
    test_ledm();
    test_snmpink();
    test_attr_line();
    test_usb_lookup();
    test_command_filter(argv[1]);
    test_maintenance_commands(argv[1]);
    puts("[PASS] Toner level suite");
    return 0;
}
