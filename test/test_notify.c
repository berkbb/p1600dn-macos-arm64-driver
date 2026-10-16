/*
 * Tests for the P1600dn Notifier core (src/p1600_notify.c): the notice
 * decisions table by table (every reason of interest with every suffix,
 * messages, toner buckets and their transitions, priming, a new cartridge,
 * the toner memory that survives restarts), and every IPP call against a
 * stand-in cupsd on a loopback port: Get-Printer-Attributes,
 * Create-Printer-Subscriptions, Get-Notifications (long poll and cupsd's
 * notify-get-interval), Renew- and Cancel-Subscription, the ReportLevels
 * command job (Create-Job, Send-Document), not-found and server-down errors.
 *
 * Usage: test_notify
 *        test_notify --serve <port file> <main|persist>
 *            the stand-in cupsd with a scripted printer for
 *            test/test_notifier_app.sh
 *        test_notify --real-snapshot <queue>
 *            one read-only Get-Printer-Attributes against this Mac's cupsd
 */
#include <arpa/inet.h>
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
#include <time.h>
#include <unistd.h>

#include "p1600_notify.h"

#define CHECK(condition)                                                                   \
    do {                                                                                   \
        if (!(condition)) {                                                                \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                       \
        }                                                                                  \
    } while (0)

#define TEST_QUEUE "P1600DN_Notify_Test"
#define P1600_OUT_OF_PAPER "Out of Paper: Refill the paper tray, then press Go on the printer."
#define P1600_MANUAL_DUPLEX \
    "Manual Duplex: For the second sides, move the printed sheets from the output bin to Tray 1, printed side down, then press Go on the printer."

static double now_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static void pause_ms(int milliseconds) {
    struct timespec pause = {milliseconds / 1000, (long)(milliseconds % 1000) * 1000000L};
    while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {
    }
}

/* --- decision logic -------------------------------------------------------- */

static P1600PrinterSnapshot make(int toner, const char *message, const char *reasons) {
    P1600PrinterSnapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    snprintf(snapshot.queue, sizeof(snapshot.queue), "Q");
    snapshot.state = 3;
    snapshot.toner = toner;
    snapshot.has_data = true;
    snprintf(snapshot.message, sizeof(snapshot.message), "%s", message ? message : "");
    if (reasons) {
        char list[512];
        snprintf(list, sizeof(list), "%s", reasons);
        char *saved = NULL;
        for (char *reason = strtok_r(list, ",", &saved); reason && snapshot.reason_count < P1600_NOTIFY_MAX_REASONS;
             reason = strtok_r(NULL, ",", &saved)) {
            snprintf(snapshot.reasons[snapshot.reason_count++], P1600_NOTIFY_REASON_SIZE, "%s", reason);
        }
    }
    return snapshot;
}

static P1600PrinterSnapshot no_data(void) {
    P1600PrinterSnapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.toner = -1;
    return snapshot;
}

static int compare(const P1600PrinterSnapshot *before, const P1600PrinterSnapshot *after, P1600Notice *notices) {
    return p1600_notify_compare(before, after, notices, P1600_NOTIFY_MAX_NOTICES);
}

static const char *const interesting[] = {
    "media-empty", "media-jam", "media-needed", "cover-open", "door-open",
    "toner-empty", "marker-supply-empty", "connecting-to-device", "offline", "other",
};
static const char *const suffixes[] = {"", "-error", "-warning", "-report"};

static void test_reasons(void) {
    char base[P1600_NOTIFY_REASON_SIZE];
    static const struct {
        const char *keyword;
        const char *base;
    } bases[] = {
        {"media-empty-error", "media-empty"}, {"media-empty-warning", "media-empty"},
        {"media-empty-report", "media-empty"}, {"media-empty", "media-empty"},
        {"media-empty-error-report", "media-empty-error"}, {"connecting-to-device-error", "connecting-to-device"},
        {"-error", "-error"}, {"error", "error"}, {"offline-report", "offline"}, {"", ""},
    };
    for (size_t i = 0; i < sizeof(bases) / sizeof(bases[0]); i++) {
        p1600_notify_reason_base(bases[i].keyword, base, sizeof(base));
        CHECK(strcmp(base, bases[i].base) == 0);
    }
    p1600_notify_reason_base(NULL, base, sizeof(base));
    CHECK(base[0] == '\0');
    p1600_notify_reason_base("media-empty-error", base, 5);
    CHECK(base[0] == '\0');
    p1600_notify_reason_base("media-empty-error", NULL, 0);

    for (size_t i = 0; i < sizeof(interesting) / sizeof(interesting[0]); i++) {
        CHECK(p1600_notify_reason_of_interest(interesting[i]));
    }
    static const char *const ignored[] = {
        "media-empty-error", "paused", "toner-low", "marker-supply-low", "media-low", "spool-area-full",
        "cups-missing-filter", "moving-to-paused", "shutdown", "timed-out", "stopping", "none", "",
        "media-empty-extra", "xmedia-empty", "Media-Empty",
    };
    for (size_t i = 0; i < sizeof(ignored) / sizeof(ignored[0]); i++) CHECK(!p1600_notify_reason_of_interest(ignored[i]));
    CHECK(!p1600_notify_reason_of_interest(NULL));

    P1600PrinterSnapshot snapshot = make(40, "", "media-jam-error");
    CHECK(p1600_notify_attention_active(&snapshot, "media-jam"));
    CHECK(!p1600_notify_attention_active(&snapshot, "media-empty"));
    CHECK(!p1600_notify_attention_active(&snapshot, NULL));
    CHECK(!p1600_notify_attention_active(NULL, "media-jam"));
    P1600PrinterSnapshot empty = no_data();
    CHECK(!p1600_notify_attention_active(&empty, "media-jam"));
    snapshot = make(40, "", "connecting-to-device");
    CHECK(!p1600_notify_attention_active(&snapshot, "connecting-to-device"));
    snapshot = make(40, "The printer is unreachable at this time.", "connecting-to-device");
    CHECK(p1600_notify_attention_active(&snapshot, "connecting-to-device"));
    snapshot = make(40, "", "connecting-to-device-error");
    CHECK(p1600_notify_attention_active(&snapshot, "connecting-to-device"));
    puts("[PASS] Reasons: suffixes stripped, the 10 reasons of interest, connecting-to-device only with -error or a message");
}

static void test_attention(void) {
    P1600Notice notices[P1600_NOTIFY_MAX_NOTICES];
    int checked = 0;
    for (size_t r = 0; r < sizeof(interesting) / sizeof(interesting[0]); r++) {
        const char *reason = interesting[r];
        const bool connecting = strcmp(reason, "connecting-to-device") == 0;
        for (size_t s = 0; s < sizeof(suffixes) / sizeof(suffixes[0]); s++) {
            for (int with_message = 0; with_message < 2; with_message++) {
                char keyword[P1600_NOTIFY_REASON_SIZE];
                snprintf(keyword, sizeof(keyword), "%s%s", reason, suffixes[s]);
                const bool error_form = strcmp(suffixes[s], "-error") == 0;
                const char *message = with_message ? "HP text: do this, then press Go." : "";
                P1600PrinterSnapshot before = make(40, "", NULL);
                P1600PrinterSnapshot after = make(40, message, keyword);
                const int expected = connecting && !error_form && !with_message ? 0 : 1;

                CHECK(compare(&before, &after, notices) == expected);
                if (expected == 1) {
                    CHECK(notices[0].kind == P1600_NOTICE_ATTENTION);
                    CHECK(strcmp(notices[0].reason, reason) == 0);
                    CHECK(strcmp(notices[0].message, message) == 0);
                    CHECK(notices[0].level == 40);
                    CHECK(notices[0].tentative == (connecting && !error_form));
                }
                /* Still there, cleared, or primed from no data (reported once). */
                CHECK(compare(&after, &after, notices) == 0);
                CHECK(compare(&after, &before, notices) == 0);
                P1600PrinterSnapshot unknown = no_data();
                CHECK(compare(&unknown, &after, notices) == expected);
                CHECK(compare(NULL, &after, notices) == expected);
                /* Another suffix of an active reason is the same reason. */
                for (size_t t = 0; t < sizeof(suffixes) / sizeof(suffixes[0]) && expected == 1; t++) {
                    char other[P1600_NOTIFY_REASON_SIZE];
                    snprintf(other, sizeof(other), "%s%s", reason, suffixes[t]);
                    P1600PrinterSnapshot changed = make(40, message, other);
                    if (connecting && !with_message && strcmp(suffixes[t], "-error") != 0) continue;
                    CHECK(compare(&after, &changed, notices) == 0);
                }
                checked++;
            }
        }
    }
    CHECK(checked == 80);

    /* Reasons that never raise a notice. */
    static const char *const quiet[] = {
        "paused", "toner-low-report", "marker-supply-low-warning", "media-low-report", "spool-area-full",
        "cups-missing-filter-warning", "moving-to-paused", "timed-out", "none", "media-empty-extra", "offline-x",
    };
    for (size_t i = 0; i < sizeof(quiet) / sizeof(quiet[0]); i++) {
        P1600PrinterSnapshot before = make(40, "", NULL);
        P1600PrinterSnapshot after = make(40, "Some message", quiet[i]);
        CHECK(compare(&before, &after, notices) == 0);
    }

    /* connecting-to-device: a message or the -error form makes it count. */
    P1600PrinterSnapshot plain = make(40, "", "connecting-to-device");
    P1600PrinterSnapshot plain_message = make(40, "Connecting to printer.", "connecting-to-device");
    P1600PrinterSnapshot engine = make(40, "Fuser Error: Turn the printer off, wait a minute, then turn it on again.", "connecting-to-device-error");
    CHECK(compare(&plain, &plain_message, notices) == 1 && notices[0].tentative);
    CHECK(strcmp(notices[0].message, "Connecting to printer.") == 0);
    CHECK(compare(&plain_message, &engine, notices) == 0);
    CHECK(compare(&plain, &engine, notices) == 1 && !notices[0].tentative);
    CHECK(strcmp(notices[0].message, "Fuser Error: Turn the printer off, wait a minute, then turn it on again.") == 0);
    CHECK(compare(&plain, &plain, notices) == 0);

    /* The message goes to the reason in the driver's -error form. */
    P1600PrinterSnapshot before = make(40, "", NULL);
    P1600PrinterSnapshot after = make(40, "Jam text", "offline-report,media-jam-error");
    CHECK(compare(&before, &after, notices) == 2);
    CHECK(strcmp(notices[0].reason, "offline") == 0 && notices[0].message[0] == '\0');
    CHECK(strcmp(notices[1].reason, "media-jam") == 0 && strcmp(notices[1].message, "Jam text") == 0);
    after = make(40, "M", "offline-report,cover-open-warning");
    CHECK(compare(&before, &after, notices) == 2);
    CHECK(strcmp(notices[0].message, "M") == 0 && notices[1].message[0] == '\0');
    before = make(40, "A", "media-empty-error");
    after = make(40, "B", "media-empty-error,media-jam-error");
    CHECK(compare(&before, &after, notices) == 1);
    CHECK(strcmp(notices[0].reason, "media-jam") == 0 && strcmp(notices[0].message, "B") == 0);
    after = make(40, "", "media-jam-error,media-jam-warning,media-jam");
    CHECK(compare(&before, &after, notices) == 1);

    /* Control characters in the message become spaces. */
    before = make(40, "", NULL);
    after = make(40, "Line one\nline\ttwo", "cover-open-error");
    CHECK(compare(&before, &after, notices) == 1 && strcmp(notices[0].message, "Line one line two") == 0);

    /* Limits and missing input. */
    after = make(4, "", "offline-report,media-jam-error,cover-open-error");
    CHECK(p1600_notify_compare(&before, &after, notices, 2) == 2);
    CHECK(p1600_notify_compare(&before, &after, notices, 3) == 3);
    CHECK(p1600_notify_compare(&before, &after, notices, 4) == 4 && notices[3].kind == P1600_NOTICE_TONER_VERY_LOW);
    CHECK(p1600_notify_compare(&before, &after, notices, 0) == 0);
    CHECK(p1600_notify_compare(&before, &after, NULL, 4) == 0);
    CHECK(p1600_notify_compare(&before, NULL, notices, 4) == 0);
    P1600PrinterSnapshot unknown = no_data();
    CHECK(p1600_notify_compare(&before, &unknown, notices, 4) == 0);
    puts("[PASS] Attention notices: 10 reasons x 4 suffixes x message, cleared, repeated, primed, routing of the message");
}

static void test_toner(void) {
    P1600Notice notices[P1600_NOTIFY_MAX_NOTICES];
    enum { NONE = -1 };
    static const struct {
        int before;
        int after;
        int kind;
    } cases[] = {
        {40, 16, NONE},
        {40, 15, P1600_NOTICE_TONER_LOW},
        {16, 15, P1600_NOTICE_TONER_LOW},
        {15, 6, NONE},
        {15, 5, P1600_NOTICE_TONER_VERY_LOW},
        {6, 5, P1600_NOTICE_TONER_VERY_LOW},
        {40, 5, P1600_NOTICE_TONER_VERY_LOW},
        {60, 3, P1600_NOTICE_TONER_VERY_LOW},
        {40, 0, P1600_NOTICE_TONER_EMPTY},
        {12, 0, P1600_NOTICE_TONER_EMPTY},
        {5, 1, NONE},
        {5, 0, P1600_NOTICE_TONER_EMPTY},
        {1, 0, P1600_NOTICE_TONER_EMPTY},
        {0, 0, NONE},
        {12, 40, NONE},
        {12, 50, NONE},
        {12, 51, P1600_NOTICE_CARTRIDGE_REPLACED},
        {15, 100, P1600_NOTICE_CARTRIDGE_REPLACED},
        {16, 100, NONE},
        {0, 60, P1600_NOTICE_CARTRIDGE_REPLACED},
        {5, 51, P1600_NOTICE_CARTRIDGE_REPLACED},
        {100, 100, NONE},
        {-1, 12, P1600_NOTICE_TONER_LOW},
        {-1, 4, P1600_NOTICE_TONER_VERY_LOW},
        {-1, 0, P1600_NOTICE_TONER_EMPTY},
        {-1, 40, NONE},
        {-1, 60, NONE},
        {12, -1, NONE},
        {-1, -1, NONE},
        {40, 101, NONE},
        {101, 12, P1600_NOTICE_TONER_LOW},
        {12, 101, NONE},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        P1600PrinterSnapshot before = make(cases[i].before, "", NULL);
        P1600PrinterSnapshot after = make(cases[i].after, "", NULL);
        int count = compare(&before, &after, notices);
        if (count != (cases[i].kind == NONE ? 0 : 1)) {
            fprintf(stderr, "toner %d -> %d gave %d notices\n", cases[i].before, cases[i].after, count);
            CHECK(0);
        }
        if (count == 1) CHECK((int)notices[0].kind == cases[i].kind && notices[0].level == cases[i].after);
        /* The first observation only primes. */
        P1600PrinterSnapshot unknown = no_data();
        CHECK(compare(&unknown, &after, notices) == 0);
    }

    static const struct {
        int level;
        const char *reasons;
        P1600TonerBucket bucket;
    } buckets[] = {
        {100, NULL, P1600_TONER_OK}, {16, NULL, P1600_TONER_OK}, {15, NULL, P1600_TONER_LOW}, {6, NULL, P1600_TONER_LOW},
        {5, NULL, P1600_TONER_VERY_LOW}, {1, NULL, P1600_TONER_VERY_LOW}, {0, NULL, P1600_TONER_EMPTY},
        {-1, NULL, P1600_TONER_UNKNOWN}, {101, NULL, P1600_TONER_UNKNOWN}, {80, "toner-empty-error", P1600_TONER_EMPTY},
        {-1, "marker-supply-empty-warning", P1600_TONER_EMPTY}, {80, "toner-low-report", P1600_TONER_OK},
    };
    for (size_t i = 0; i < sizeof(buckets) / sizeof(buckets[0]); i++) {
        P1600PrinterSnapshot snapshot = make(buckets[i].level, "", buckets[i].reasons);
        CHECK(p1600_notify_toner_bucket(&snapshot) == buckets[i].bucket);
    }
    P1600PrinterSnapshot unknown = no_data();
    unknown.toner = 40;
    CHECK(p1600_notify_toner_bucket(&unknown) == P1600_TONER_UNKNOWN && p1600_notify_toner_bucket(NULL) == P1600_TONER_UNKNOWN);

    /* Toner reasons: the attention notice stands for the empty bucket. */
    P1600PrinterSnapshot before = make(40, "", NULL);
    P1600PrinterSnapshot after = make(40, "Toner Missing: ...", "toner-empty-error");
    CHECK(compare(&before, &after, notices) == 1 && notices[0].kind == P1600_NOTICE_ATTENTION);
    CHECK(strcmp(notices[0].reason, "toner-empty") == 0);
    after = make(0, "", "marker-supply-empty-warning");
    CHECK(compare(&before, &after, notices) == 1 && strcmp(notices[0].reason, "marker-supply-empty") == 0);
    before = make(0, "", NULL);
    after = make(0, "", "toner-empty-error");
    CHECK(compare(&before, &after, notices) == 1 && notices[0].kind == P1600_NOTICE_ATTENTION);
    before = make(40, "", "toner-empty-error");
    after = make(40, "", NULL);
    CHECK(compare(&before, &after, notices) == 0);
    after = make(0, "", NULL);
    CHECK(compare(&before, &after, notices) == 0);
    before = make(10, "", "toner-empty-error");
    after = make(100, "", NULL);
    CHECK(compare(&before, &after, notices) == 1 && notices[0].kind == P1600_NOTICE_CARTRIDGE_REPLACED);
    CHECK(notices[0].level == 100);

    /* Attention and toner together: attention first. */
    before = make(40, "", NULL);
    after = make(4, P1600_OUT_OF_PAPER, "media-empty-error");
    CHECK(compare(&before, &after, notices) == 2);
    CHECK(notices[0].kind == P1600_NOTICE_ATTENTION && strcmp(notices[0].message, P1600_OUT_OF_PAPER) == 0);
    CHECK(notices[1].kind == P1600_NOTICE_TONER_VERY_LOW && notices[1].level == 4 && notices[1].message[0] == '\0');
    CHECK(p1600_notify_compare(&before, &after, notices, 1) == 1 && notices[0].kind == P1600_NOTICE_ATTENTION);
    puts("[PASS] Toner: buckets 15/5/0 and reasons, worse buckets only, unknown counts as ok, new cartridge, priming");
}

static void test_memory(void) {
    P1600Notice low = {.kind = P1600_NOTICE_TONER_LOW};
    P1600Notice very_low = {.kind = P1600_NOTICE_TONER_VERY_LOW};
    P1600Notice empty = {.kind = P1600_NOTICE_TONER_EMPTY};
    P1600Notice replaced = {.kind = P1600_NOTICE_CARTRIDGE_REPLACED};
    P1600Notice attention = {.kind = P1600_NOTICE_ATTENTION, .reason = "media-jam"};
    CHECK(p1600_notify_should_post(&low, P1600_TONER_UNKNOWN) && p1600_notify_should_post(&low, P1600_TONER_OK));
    CHECK(!p1600_notify_should_post(&low, P1600_TONER_LOW) && !p1600_notify_should_post(&low, P1600_TONER_VERY_LOW));
    CHECK(!p1600_notify_should_post(&low, P1600_TONER_EMPTY));
    CHECK(p1600_notify_should_post(&very_low, P1600_TONER_LOW) && !p1600_notify_should_post(&very_low, P1600_TONER_VERY_LOW));
    CHECK(p1600_notify_should_post(&empty, P1600_TONER_VERY_LOW) && !p1600_notify_should_post(&empty, P1600_TONER_EMPTY));
    CHECK(p1600_notify_should_post(&replaced, P1600_TONER_OK) && p1600_notify_should_post(&replaced, P1600_TONER_EMPTY));
    CHECK(p1600_notify_should_post(&attention, P1600_TONER_EMPTY));
    CHECK(p1600_notify_should_post(&empty, (P1600TonerBucket)7) && p1600_notify_should_post(&low, (P1600TonerBucket)-5));
    CHECK(!p1600_notify_should_post(NULL, P1600_TONER_OK));

    P1600PrinterSnapshot twelve = make(12, "", NULL);
    P1600PrinterSnapshot unknown_level = make(-1, "", NULL);
    P1600PrinterSnapshot sixty = make(60, "", NULL);
    P1600PrinterSnapshot missing = make(40, "", "toner-empty-error");
    P1600PrinterSnapshot nothing = no_data();
    CHECK(p1600_notify_remember(P1600_TONER_UNKNOWN, &twelve) == P1600_TONER_LOW);
    CHECK(p1600_notify_remember(P1600_TONER_LOW, &unknown_level) == P1600_TONER_LOW);
    CHECK(p1600_notify_remember(P1600_TONER_EMPTY, &sixty) == P1600_TONER_OK);
    CHECK(p1600_notify_remember(P1600_TONER_LOW, &nothing) == P1600_TONER_LOW);
    CHECK(p1600_notify_remember(P1600_TONER_LOW, &missing) == P1600_TONER_EMPTY);
    CHECK(p1600_notify_remember((P1600TonerBucket)9, &unknown_level) == P1600_TONER_UNKNOWN);
    CHECK(p1600_notify_remember(P1600_TONER_OK, NULL) == P1600_TONER_OK);

    P1600PrinterSnapshot after = make(-1, "", NULL);
    p1600_notify_carry_toner(&after, &twelve);
    CHECK(after.toner == 12);
    after = make(30, "", NULL);
    p1600_notify_carry_toner(&after, &twelve);
    CHECK(after.toner == 30);
    after = make(-1, "", NULL);
    p1600_notify_carry_toner(&after, &unknown_level);
    CHECK(after.toner == -1);
    p1600_notify_carry_toner(&after, &nothing);
    CHECK(after.toner == -1);
    nothing = no_data();
    p1600_notify_carry_toner(&nothing, &twelve);
    CHECK(nothing.toner == -1);
    p1600_notify_carry_toner(NULL, &twelve);

    /* A restart: the agent saw "low" before; marker-levels are unknown at
     * first, then 12% again: no repeat. Then 4%: very low is new. */
    P1600Notice notices[P1600_NOTIFY_MAX_NOTICES];
    P1600TonerBucket memory = P1600_TONER_LOW;
    P1600PrinterSnapshot first = make(-1, "", NULL);
    CHECK(compare(&nothing, &first, notices) == 0);
    memory = p1600_notify_remember(memory, &first);
    CHECK(memory == P1600_TONER_LOW);
    P1600PrinterSnapshot second = make(12, "", NULL);
    CHECK(compare(&first, &second, notices) == 1 && notices[0].kind == P1600_NOTICE_TONER_LOW);
    CHECK(!p1600_notify_should_post(&notices[0], memory));
    memory = p1600_notify_remember(memory, &second);
    P1600PrinterSnapshot third = make(4, "", NULL);
    CHECK(compare(&second, &third, notices) == 1 && p1600_notify_should_post(&notices[0], memory));
    CHECK(p1600_notify_remember(memory, &third) == P1600_TONER_VERY_LOW);

    /* Reasons remembered across restarts. */
    char list[256];
    P1600PrinterSnapshot active = make(40, "", "paused,offline-report,media-jam-error,connecting-to-device,media-jam");
    CHECK(p1600_notify_active_reasons(&active, list, sizeof(list)) == strlen("media-jam,offline"));
    CHECK(strcmp(list, "media-jam,offline") == 0);
    CHECK(p1600_notify_active_reasons(&active, list, 9) == 0 && list[0] == '\0');
    P1600PrinterSnapshot all = make(40, "Message", "other,offline-report,connecting-to-device,marker-supply-empty-warning,"
                                 "toner-empty-error,door-open,cover-open,media-needed,media-jam,media-empty");
    CHECK(p1600_notify_active_reasons(&all, list, sizeof(list)) > 0);
    CHECK(strcmp(list, "media-empty,media-jam,media-needed,cover-open,door-open,toner-empty,marker-supply-empty,"
                       "connecting-to-device,offline,other") == 0);
    CHECK(p1600_notify_active_reasons(&nothing, list, sizeof(list)) == 0 && list[0] == '\0');
    CHECK(p1600_notify_active_reasons(NULL, list, sizeof(list)) == 0 && p1600_notify_active_reasons(&all, NULL, 4) == 0);
    CHECK(p1600_notify_reason_listed("media-jam,offline", "media-jam") && p1600_notify_reason_listed("media-jam,offline", "offline"));
    CHECK(!p1600_notify_reason_listed("media-jam,offline", "media") && !p1600_notify_reason_listed("media-jam,offline", "jam"));
    CHECK(!p1600_notify_reason_listed("media-jam,offline", "") && !p1600_notify_reason_listed(NULL, "offline"));
    CHECK(!p1600_notify_reason_listed("", "offline") && p1600_notify_reason_listed("offline", "offline"));
    CHECK(!p1600_notify_reason_listed("media-jam,offline", NULL) && p1600_notify_reason_listed(",offline,", "offline"));
    /* A restart while the stack waits for the Go button: not again. */
    P1600PrinterSnapshot waiting = make(40, P1600_MANUAL_DUPLEX, "media-needed-error");
    CHECK(compare(&nothing, &waiting, notices) == 1);
    CHECK(p1600_notify_reason_listed("media-needed", notices[0].reason));

    char name[64];
    CHECK(p1600_notice_name(&attention, name, sizeof(name)) == strlen("attention.media-jam"));
    CHECK(strcmp(name, "attention.media-jam") == 0);
    p1600_notice_name(&low, name, sizeof(name));
    CHECK(strcmp(name, "toner.low") == 0);
    p1600_notice_name(&very_low, name, sizeof(name));
    CHECK(strcmp(name, "toner.very-low") == 0);
    p1600_notice_name(&empty, name, sizeof(name));
    CHECK(strcmp(name, "toner.empty") == 0);
    p1600_notice_name(&replaced, name, sizeof(name));
    CHECK(strcmp(name, "toner.replaced") == 0);
    CHECK(p1600_notice_name(&attention, name, 8) == 0 && name[0] == '\0');
    P1600Notice invalid = {.kind = (P1600NoticeKind)99};
    CHECK(p1600_notice_name(&invalid, name, sizeof(name)) == 0 && name[0] == '\0');
    CHECK(p1600_notice_name(NULL, name, sizeof(name)) == 0 && p1600_notice_name(&low, NULL, 0) == 0);
    puts("[PASS] Memory across restarts: toner bucket, reported reasons; unknown levels carried; notice names");
}

/* --- stand-in cupsd ---------------------------------------------------------- */

#define MAX_PRINTERS 4
#define MAX_SUBSCRIPTIONS 32
#define MAX_EVENTS 512
#define MAX_CONNECTIONS 32
#define MAX_REASONS 20

typedef struct {
    char name[128];
    char info[256];
    int state;
    int reason_count;
    char reasons[MAX_REASONS][80];
    char message[512];
    int marker_count;
    int levels[3];
    char types[3][32];
    char names[3][32];
} FakePrinter;

typedef struct {
    int id;
    int printer;
    int lease;
    bool active;
    int next_sequence;
} FakeSubscription;

typedef struct {
    int subscription;
    int sequence;
    int printer;
    int job_id;
    char event[40];
} FakeEvent;

typedef struct Standin Standin;

typedef struct {
    Standin *standin;
    http_t *http;
    pthread_t thread;
    atomic_bool done;
} Connection;

struct Standin {
    int listener;
    int port;
    pthread_t acceptor;
    atomic_bool stop;
    Connection *connections[MAX_CONNECTIONS];  /* acceptor thread only */
    pthread_mutex_t lock;
    pthread_cond_t changed;
    /* Everything below is guarded by lock. */
    FakePrinter printers[MAX_PRINTERS];
    int printer_count;
    FakeSubscription subscriptions[MAX_SUBSCRIPTIONS];
    int subscription_count;
    FakeEvent events[MAX_EVENTS];
    int event_count;
    /* Behaviour. */
    bool cupsd_mode;          /* answer Get-Notifications at once with notify-get-interval */
    int interval;
    int hold_ms;              /* notify-wait: hold an empty answer this long */
    int max_lease;
    bool refuse_subscriptions;
    bool replay_one;          /* also send the event before the asked number */
    bool events_complete;
    void (*on_command)(Standin *standin, int printer);  /* serve mode, called with lock held */
    /* Records. */
    int operations[0x40];
    int snapshots;            /* Get-Printer-Attributes asking for exactly the notifier's 7 */
    char printer_uri[256];
    char user[128];
    char requested[16][48];
    int requested_count;
    char notify_events[8][48];
    int notify_event_count;
    char pull_method[32];
    int subscribe_lease;
    int renew_lease;
    int asked_sequence;
    bool asked_wait;
    int next_job_id;
    int job_ids[8];
    int job_count;
    char job_name[128];
    char document_format[64];
    char document_name[128];
    bool last_document;
    int document_job;
    unsigned char document[256];
    size_t document_length;
};

static void standin_lock(Standin *standin) {
    CHECK(pthread_mutex_lock(&standin->lock) == 0);
}

static void standin_unlock(Standin *standin) {
    CHECK(pthread_mutex_unlock(&standin->lock) == 0);
}

static int find_printer(const Standin *standin, const char *name) {
    for (int i = 0; i < standin->printer_count; i++) {
        if (strcmp(standin->printers[i].name, name) == 0) return i;
    }
    return -1;
}

static FakeSubscription *find_subscription(Standin *standin, int id) {
    for (int i = 0; i < standin->subscription_count; i++) {
        if (standin->subscriptions[i].id == id && standin->subscriptions[i].active) return &standin->subscriptions[i];
    }
    return NULL;
}

/* Queues an event for every active subscription of the printer. */
static void push_event_locked(Standin *standin, int printer, const char *event, int job_id) {
    for (int i = 0; i < standin->subscription_count; i++) {
        FakeSubscription *subscription = &standin->subscriptions[i];
        if (!subscription->active || subscription->printer != printer || standin->event_count >= MAX_EVENTS) continue;
        FakeEvent *queued = &standin->events[standin->event_count++];
        memset(queued, 0, sizeof(*queued));
        queued->subscription = subscription->id;
        queued->sequence = subscription->next_sequence++;
        queued->printer = printer;
        queued->job_id = job_id;
        snprintf(queued->event, sizeof(queued->event), "%s", event);
    }
    pthread_cond_broadcast(&standin->changed);
}

static void push_event(Standin *standin, const char *queue, const char *event, int job_id) {
    standin_lock(standin);
    int printer = find_printer(standin, queue);
    CHECK(printer >= 0);
    push_event_locked(standin, printer, event, job_id);
    standin_unlock(standin);
}

static FakePrinter *printer_locked(Standin *standin, const char *queue) {
    int index = find_printer(standin, queue);
    if (index < 0) {
        CHECK(standin->printer_count < MAX_PRINTERS);
        index = standin->printer_count++;
        memset(&standin->printers[index], 0, sizeof(standin->printers[index]));
        snprintf(standin->printers[index].name, sizeof(standin->printers[index].name), "%s", queue);
        standin->printers[index].state = 3;
    }
    return &standin->printers[index];
}

static void set_markers_locked(FakePrinter *printer, int toner) {
    printer->marker_count = toner == -100 ? 0 : 1;
    printer->levels[0] = toner;
    snprintf(printer->types[0], sizeof(printer->types[0]), "toner");
    snprintf(printer->names[0], sizeof(printer->names[0]), "Black");
}

/* Sets a printer's state ("a,b" reasons, NULL keeps them; toner -100 removes
 * the markers, -99 keeps them) and queues printer-state-changed. */
static void set_printer(Standin *standin, const char *queue, int state, const char *reasons, const char *message,
                        int toner) {
    standin_lock(standin);
    FakePrinter *printer = printer_locked(standin, queue);
    if (state > 0) printer->state = state;
    if (reasons) {
        printer->reason_count = 0;
        char list[512];
        snprintf(list, sizeof(list), "%s", reasons);
        char *saved = NULL;
        for (char *reason = strtok_r(list, ",", &saved); reason && printer->reason_count < MAX_REASONS;
             reason = strtok_r(NULL, ",", &saved)) {
            snprintf(printer->reasons[printer->reason_count++], sizeof(printer->reasons[0]), "%s", reason);
        }
    }
    if (message) snprintf(printer->message, sizeof(printer->message), "%s", message);
    if (toner != -99) set_markers_locked(printer, toner);
    push_event_locked(standin, (int)(printer - standin->printers), "printer-state-changed", 0);
    standin_unlock(standin);
}

static void decode_path(const char *path, char *out, size_t size) {
    size_t length = 0;
    for (const char *p = path; *p && *p != '?' && length + 1U < size; p++) {
        unsigned int value = 0;
        if (p[0] == '%' && sscanf(p + 1, "%2x", &value) == 1) {
            out[length++] = (char)value;
            p += 2;
        } else {
            out[length++] = *p;
        }
    }
    out[length] = '\0';
}

static int request_printer(Standin *standin, ipp_t *request) {
    ipp_attribute_t *uri = ippFindAttribute(request, "printer-uri", IPP_TAG_URI);
    const char *text = uri ? ippGetString(uri, 0, NULL) : NULL;
    const char *path = text ? strstr(text, "/printers/") : NULL;
    if (!path) return -1;
    char name[128];
    decode_path(path + strlen("/printers/"), name, sizeof(name));
    return find_printer(standin, name);
}

static int request_integer(ipp_t *request, const char *name, int fallback) {
    ipp_attribute_t *attribute = ippFindAttribute(request, name, IPP_TAG_INTEGER);
    return attribute ? ippGetInteger(attribute, 0) : fallback;
}

static void add_printer_attributes(ipp_t *response, const FakePrinter *printer) {
    char uri[512];
    httpAssembleURIf(HTTP_URI_CODING_ALL, uri, sizeof(uri), "ipp", NULL, "localhost", 0, "/printers/%s",
                     printer->name);
    ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_URI, "printer-uri-supported", NULL, uri);
    ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_NAME, "printer-name", NULL, printer->name);
    ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_TEXT, "printer-info", NULL, printer->info);
    ippAddInteger(response, IPP_TAG_PRINTER, IPP_TAG_ENUM, "printer-state", printer->state);
    const char *reasons[MAX_REASONS];
    for (int i = 0; i < printer->reason_count; i++) reasons[i] = printer->reasons[i];
    if (printer->reason_count == 0) {
        ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "printer-state-reasons", NULL, "none");
    } else {
        ippAddStrings(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "printer-state-reasons", printer->reason_count,
                      NULL, reasons);
    }
    ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_TEXT, "printer-state-message", NULL, printer->message);
    if (printer->marker_count > 0) {
        const char *types[3], *names[3];
        for (int i = 0; i < printer->marker_count; i++) {
            types[i] = printer->types[i];
            names[i] = printer->names[i];
        }
        ippAddIntegers(response, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "marker-levels", printer->marker_count,
                       printer->levels);
        ippAddStrings(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "marker-types", printer->marker_count, NULL, types);
        ippAddStrings(response, IPP_TAG_PRINTER, IPP_TAG_NAME, "marker-names", printer->marker_count, NULL, names);
    }
}

static void add_event(ipp_t *response, const Standin *standin, const FakeEvent *event, bool first) {
    if (!first) ippAddSeparator(response);
    const FakePrinter *printer = &standin->printers[event->printer];
    char uri[512];
    httpAssembleURIf(HTTP_URI_CODING_ALL, uri, sizeof(uri), "ipp", NULL, "localhost", 0, "/printers/%s",
                     printer->name);
    ippAddInteger(response, IPP_TAG_EVENT_NOTIFICATION, IPP_TAG_INTEGER, "notify-subscription-id",
                  event->subscription);
    ippAddString(response, IPP_TAG_EVENT_NOTIFICATION, IPP_TAG_URI, "notify-printer-uri", NULL, uri);
    ippAddString(response, IPP_TAG_EVENT_NOTIFICATION, IPP_TAG_KEYWORD, "notify-subscribed-event", NULL,
                 event->event);
    ippAddInteger(response, IPP_TAG_EVENT_NOTIFICATION, IPP_TAG_INTEGER, "notify-sequence-number", event->sequence);
    ippAddString(response, IPP_TAG_EVENT_NOTIFICATION, IPP_TAG_TEXT, "notify-text", NULL, "Printer state changed.");
    if (strncmp(event->event, "job-", 4) == 0) {
        /* Job events name the printer only by URI here. */
        ippAddInteger(response, IPP_TAG_EVENT_NOTIFICATION, IPP_TAG_INTEGER, "notify-job-id", event->job_id);
        ippAddInteger(response, IPP_TAG_EVENT_NOTIFICATION, IPP_TAG_ENUM, "job-state", IPP_JSTATE_COMPLETED);
    } else {
        ippAddString(response, IPP_TAG_EVENT_NOTIFICATION, IPP_TAG_NAME, "printer-name", NULL, printer->name);
        ippAddInteger(response, IPP_TAG_EVENT_NOTIFICATION, IPP_TAG_ENUM, "printer-state", printer->state);
    }
}

static int collect_events(Standin *standin, ipp_t *response, int id, int first) {
    int count = 0, added = 0;
    for (int i = 0; i < standin->event_count; i++) {
        const FakeEvent *event = &standin->events[i];
        if (event->subscription != id) continue;
        if (event->sequence >= first || (standin->replay_one && event->sequence == first - 1)) {
            add_event(response, standin, event, added++ == 0);
            if (event->sequence >= first) count++;
        }
    }
    return count;
}

static bool has_new_events(const Standin *standin, int id, int first) {
    for (int i = 0; i < standin->event_count; i++) {
        if (standin->events[i].subscription == id && standin->events[i].sequence >= first) return true;
    }
    return false;
}

/* Answers one IPP request (lock held; document data already read). */
static void handle_request(Standin *standin, ipp_t *request, ipp_t *response) {
    ipp_op_t operation = ippGetOperation(request);
    if ((int)operation >= 0 && (int)operation < 0x40) standin->operations[operation]++;
    ipp_attribute_t *uri = ippFindAttribute(request, "printer-uri", IPP_TAG_URI);
    snprintf(standin->printer_uri, sizeof(standin->printer_uri), "%s", uri ? ippGetString(uri, 0, NULL) : "");
    ipp_attribute_t *user = ippFindAttribute(request, "requesting-user-name", IPP_TAG_NAME);
    snprintf(standin->user, sizeof(standin->user), "%s", user ? ippGetString(user, 0, NULL) : "");
    ippSetStatusCode(response, IPP_STATUS_OK);
    int printer = request_printer(standin, request);

    switch (operation) {
    case IPP_OP_GET_PRINTER_ATTRIBUTES: {
        ipp_attribute_t *requested = ippFindAttribute(request, "requested-attributes", IPP_TAG_KEYWORD);
        standin->requested_count = 0;
        for (int i = 0; requested && i < ippGetCount(requested) && i < 16; i++) {
            snprintf(standin->requested[standin->requested_count++], sizeof(standin->requested[0]), "%s",
                     ippGetString(requested, i, NULL));
        }
        if (printer < 0) {
            ippSetStatusCode(response, IPP_STATUS_ERROR_NOT_FOUND);
            break;
        }
        if (standin->requested_count == 7 && strcmp(standin->requested[5], "printer-state-message") == 0) {
            standin->snapshots++;
        }
        add_printer_attributes(response, &standin->printers[printer]);
        break;
    }
    case IPP_OP_CREATE_PRINTER_SUBSCRIPTIONS: {
        ipp_attribute_t *events = ippFindAttribute(request, "notify-events", IPP_TAG_KEYWORD);
        standin->notify_event_count = 0;
        for (int i = 0; events && i < ippGetCount(events) && i < 8; i++) {
            snprintf(standin->notify_events[standin->notify_event_count++], sizeof(standin->notify_events[0]), "%s",
                     ippGetString(events, i, NULL));
        }
        ipp_attribute_t *method = ippFindAttribute(request, "notify-pull-method", IPP_TAG_KEYWORD);
        snprintf(standin->pull_method, sizeof(standin->pull_method), "%s", method ? ippGetString(method, 0, NULL) : "");
        standin->subscribe_lease = request_integer(request, "notify-lease-duration", -1);
        if (printer < 0) {
            ippSetStatusCode(response, IPP_STATUS_ERROR_NOT_FOUND);
            break;
        }
        if (standin->refuse_subscriptions || standin->subscription_count >= MAX_SUBSCRIPTIONS) {
            ippSetStatusCode(response, IPP_STATUS_ERROR_NOT_POSSIBLE);
            break;
        }
        FakeSubscription *subscription = &standin->subscriptions[standin->subscription_count++];
        static int next_id = 40;
        subscription->id = ++next_id;
        subscription->printer = printer;
        subscription->active = true;
        subscription->next_sequence = 1;
        int lease = standin->subscribe_lease;
        if (standin->max_lease > 0 && (lease == 0 || lease > standin->max_lease)) lease = standin->max_lease;
        subscription->lease = lease;
        ippAddInteger(response, IPP_TAG_SUBSCRIPTION, IPP_TAG_INTEGER, "notify-subscription-id", subscription->id);
        ippAddInteger(response, IPP_TAG_SUBSCRIPTION, IPP_TAG_INTEGER, "notify-lease-duration", lease);
        break;
    }
    case IPP_OP_GET_NOTIFICATIONS: {
        int id = request_integer(request, "notify-subscription-ids", 0);
        int first = request_integer(request, "notify-sequence-numbers", 1);
        ipp_attribute_t *wait = ippFindAttribute(request, "notify-wait", IPP_TAG_BOOLEAN);
        standin->asked_sequence = first;
        standin->asked_wait = wait && ippGetBoolean(wait, 0);
        if (!find_subscription(standin, id)) {
            ippSetStatusCode(response, IPP_STATUS_ERROR_NOT_FOUND);
            break;
        }
        if (standin->events_complete) {
            ippSetStatusCode(response, IPP_STATUS_OK_EVENTS_COMPLETE);
            break;
        }
        if (standin->cupsd_mode) {
            ippAddInteger(response, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "notify-get-interval", standin->interval);
        } else if (standin->asked_wait) {
            /* Long poll: hold the answer until an event arrives. */
            double deadline = now_seconds() + standin->hold_ms / 1000.0;
            while (!has_new_events(standin, id, first) && !atomic_load(&standin->stop)) {
                double left = deadline - now_seconds();
                if (left <= 0.0) break;
                if (left > 0.05) left = 0.05;
                struct timespec relative = {0, (long)(left * 1e9)};
                pthread_cond_timedwait_relative_np(&standin->changed, &standin->lock, &relative);
            }
            if (!find_subscription(standin, id)) {
                ippSetStatusCode(response, IPP_STATUS_ERROR_NOT_FOUND);
                break;
            }
        }
        ippAddInteger(response, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "printer-up-time", (int)time(NULL));
        collect_events(standin, response, id, first);
        break;
    }
    case IPP_OP_RENEW_SUBSCRIPTION: {
        FakeSubscription *subscription = find_subscription(standin, request_integer(request, "notify-subscription-id", 0));
        standin->renew_lease = request_integer(request, "notify-lease-duration", -1);
        if (!subscription) {
            ippSetStatusCode(response, IPP_STATUS_ERROR_NOT_FOUND);
            break;
        }
        int lease = standin->renew_lease;
        if (standin->max_lease > 0 && (lease == 0 || lease > standin->max_lease)) lease = standin->max_lease;
        subscription->lease = lease;
        ippAddInteger(response, IPP_TAG_SUBSCRIPTION, IPP_TAG_INTEGER, "notify-lease-duration", lease);
        break;
    }
    case IPP_OP_CANCEL_SUBSCRIPTION: {
        FakeSubscription *subscription = find_subscription(standin, request_integer(request, "notify-subscription-id", 0));
        if (!subscription) {
            ippSetStatusCode(response, IPP_STATUS_ERROR_NOT_FOUND);
            break;
        }
        subscription->active = false;
        break;
    }
    case IPP_OP_CREATE_JOB: {
        ipp_attribute_t *name = ippFindAttribute(request, "job-name", IPP_TAG_NAME);
        snprintf(standin->job_name, sizeof(standin->job_name), "%s", name ? ippGetString(name, 0, NULL) : "");
        if (printer < 0) {
            ippSetStatusCode(response, IPP_STATUS_ERROR_NOT_FOUND);
            break;
        }
        int job_id = ++standin->next_job_id;
        if (standin->job_count < 8) standin->job_ids[standin->job_count++] = job_id;
        ippAddInteger(response, IPP_TAG_JOB, IPP_TAG_INTEGER, "job-id", job_id);
        ippAddInteger(response, IPP_TAG_JOB, IPP_TAG_ENUM, "job-state", IPP_JSTATE_HELD);
        break;
    }
    case IPP_OP_SEND_DOCUMENT: {
        int job_id = request_integer(request, "job-id", 0);
        ipp_attribute_t *format = ippFindAttribute(request, "document-format", IPP_TAG_MIMETYPE);
        ipp_attribute_t *name = ippFindAttribute(request, "document-name", IPP_TAG_NAME);
        ipp_attribute_t *last = ippFindAttribute(request, "last-document", IPP_TAG_BOOLEAN);
        snprintf(standin->document_format, sizeof(standin->document_format), "%s",
                 format ? ippGetString(format, 0, NULL) : "");
        snprintf(standin->document_name, sizeof(standin->document_name), "%s", name ? ippGetString(name, 0, NULL) : "");
        standin->last_document = last && ippGetBoolean(last, 0);
        standin->document_job = job_id;
        bool known = false;
        for (int i = 0; i < standin->job_count; i++) known = known || standin->job_ids[i] == job_id;
        if (printer < 0 || !known) {
            ippSetStatusCode(response, IPP_STATUS_ERROR_NOT_FOUND);
            break;
        }
        ippAddInteger(response, IPP_TAG_JOB, IPP_TAG_INTEGER, "job-id", job_id);
        ippAddInteger(response, IPP_TAG_JOB, IPP_TAG_ENUM, "job-state", IPP_JSTATE_PENDING);
        if (standin->on_command) standin->on_command(standin, printer);
        break;
    }
    case IPP_OP_CANCEL_JOB:
        break;
    default:
        ippSetStatusCode(response, IPP_STATUS_ERROR_OPERATION_NOT_SUPPORTED);
        break;
    }
    pthread_cond_broadcast(&standin->changed);
}

/* ippReadIO/ippWriteIO need the whole amount from each call. */
static ssize_t read_http(void *context, ipp_uchar_t *buffer, size_t length) {
    size_t total = 0;
    while (total < length) {
        ssize_t received = httpRead2((http_t *)context, (char *)buffer + total, length - total);
        if (received <= 0) return total > 0 ? (ssize_t)total : received;
        total += (size_t)received;
    }
    return (ssize_t)total;
}

static ssize_t write_http(void *context, ipp_uchar_t *buffer, size_t length) {
    return httpWrite2((http_t *)context, (const char *)buffer, length);
}

static bool serve_one(Standin *standin, http_t *http) {
    char resource[1024];
    http_state_t state = httpReadRequest(http, resource, sizeof(resource));
    if (state == HTTP_STATE_WAITING) return true;
    if (state != HTTP_STATE_POST) return false;
    http_status_t status;
    while ((status = httpUpdate(http)) == HTTP_STATUS_CONTINUE) {
    }
    if (status != HTTP_STATUS_OK) return false;
    if (httpGetExpect(http) == HTTP_STATUS_CONTINUE) {
        httpClearFields(http);
        if (httpWriteResponse(http, HTTP_STATUS_CONTINUE) < 0) return false;
    }

    ipp_t *request = ippNew();
    ipp_state_t ipp_state;
    while ((ipp_state = ippReadIO(http, read_http, 1, NULL, request)) != IPP_STATE_DATA) {
        if (ipp_state == IPP_STATE_ERROR) {
            ippDelete(request);
            return false;
        }
    }
    /* Document data (the ReportLevels command file). */
    unsigned char data[256];
    size_t data_length = 0;
    if (ippGetOperation(request) == IPP_OP_SEND_DOCUMENT) {
        ssize_t received;
        while ((received = httpRead2(http, (char *)data + data_length, sizeof(data) - data_length)) > 0) {
            data_length += (size_t)received;
            if (data_length == sizeof(data)) break;
        }
    }
    if (httpGetState(http) != HTTP_STATE_POST_SEND) httpFlush(http);

    ipp_t *response = ippNewResponse(request);
    standin_lock(standin);
    if (ippGetOperation(request) == IPP_OP_SEND_DOCUMENT) {
        memcpy(standin->document, data, data_length);
        standin->document_length = data_length;
    }
    handle_request(standin, request, response);
    standin_unlock(standin);

    httpClearFields(http);
    httpSetField(http, HTTP_FIELD_CONTENT_TYPE, "application/ipp");
    httpSetLength(http, ippLength(response));
    bool ok = httpWriteResponse(http, HTTP_STATUS_OK) == 0;
    ippSetState(response, IPP_STATE_IDLE);
    while (ok && (ipp_state = ippWriteIO(http, write_http, 1, NULL, response)) != IPP_STATE_DATA) {
        if (ipp_state == IPP_STATE_ERROR) ok = false;
    }
    if (ok) httpFlushWrite(http);
    ippDelete(request);
    ippDelete(response);
    return ok;
}

static void *connection_main(void *argument) {
    Connection *connection = argument;
    while (!atomic_load(&connection->standin->stop)) {
        if (!httpWait(connection->http, 100)) continue;
        if (!serve_one(connection->standin, connection->http)) break;
    }
    httpClose(connection->http);
    atomic_store(&connection->done, true);
    return NULL;
}

static void reap_connections(Standin *standin, bool all) {
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        Connection *connection = standin->connections[i];
        if (!connection || (!all && !atomic_load(&connection->done))) continue;
        pthread_join(connection->thread, NULL);
        free(connection);
        standin->connections[i] = NULL;
    }
}

static void *acceptor_main(void *argument) {
    Standin *standin = argument;
    while (!atomic_load(&standin->stop)) {
        struct pollfd channel = {.fd = standin->listener, .events = POLLIN};
        if (poll(&channel, 1, 50) <= 0) continue;
        reap_connections(standin, false);
        http_t *http = httpAcceptConnection(standin->listener, 1);
        if (!http) continue;
        int slot = -1;
        for (int i = 0; i < MAX_CONNECTIONS && slot < 0; i++) {
            if (!standin->connections[i]) slot = i;
        }
        Connection *connection = slot >= 0 ? calloc(1, sizeof(*connection)) : NULL;
        if (!connection) {
            httpClose(http);
            continue;
        }
        connection->standin = standin;
        connection->http = http;
        atomic_init(&connection->done, false);
        if (pthread_create(&connection->thread, NULL, connection_main, connection) != 0) {
            httpClose(http);
            free(connection);
            continue;
        }
        standin->connections[slot] = connection;
    }
    reap_connections(standin, true);
    return NULL;
}

/* Starts the stand-in on 127.0.0.1:`port` (0: any free port). The printer
 * and subscription tables start empty, like a freshly started cupsd. */
static void standin_start(Standin *standin, int port) {
    memset(standin, 0, sizeof(*standin));
    atomic_init(&standin->stop, false);
    standin->hold_ms = 300;
    standin->interval = 1;
    standin->listener = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(standin->listener >= 0);
    int yes = 1;
    setsockopt(standin->listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t length = sizeof(address);
    CHECK(bind(standin->listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    CHECK(listen(standin->listener, 16) == 0);
    CHECK(getsockname(standin->listener, (struct sockaddr *)&address, &length) == 0);
    standin->port = ntohs(address.sin_port);
    CHECK(pthread_mutex_init(&standin->lock, NULL) == 0);
    CHECK(pthread_cond_init(&standin->changed, NULL) == 0);
    CHECK(pthread_create(&standin->acceptor, NULL, acceptor_main, standin) == 0);
}

static void standin_stop(Standin *standin) {
    atomic_store(&standin->stop, true);
    standin_lock(standin);
    pthread_cond_broadcast(&standin->changed);
    standin_unlock(standin);
    pthread_join(standin->acceptor, NULL);
    close(standin->listener);
    pthread_cond_destroy(&standin->changed);
    pthread_mutex_destroy(&standin->lock);
}

static int operation_count(Standin *standin, ipp_op_t operation) {
    standin_lock(standin);
    int count = standin->operations[operation];
    standin_unlock(standin);
    return count;
}

/* --- IPP tests ------------------------------------------------------------- */

static void test_snapshot(Standin *standin) {
    set_printer(standin, TEST_QUEUE, 3, "", "", 40);
    standin_lock(standin);
    snprintf(standin->printers[0].info, sizeof(standin->printers[0].info), "Office P1606dn");
    standin_unlock(standin);

    http_t *http = p1600_notify_connect(5000);
    CHECK(http);
    P1600PrinterSnapshot snapshot;
    CHECK(p1600_notify_snapshot(http, TEST_QUEUE, &snapshot) == P1600_NOTIFY_OK);
    CHECK(snapshot.has_data && snapshot.state == 3 && snapshot.reason_count == 0 && snapshot.toner == 40);
    CHECK(strcmp(snapshot.queue, TEST_QUEUE) == 0 && strcmp(snapshot.info, "Office P1606dn") == 0);
    CHECK(snapshot.message[0] == '\0');
    standin_lock(standin);
    CHECK(strcmp(standin->printer_uri, "ipp://localhost/printers/" TEST_QUEUE) == 0);
    CHECK(standin->user[0] != '\0');
    static const char *const expected[] = {
        "marker-levels", "marker-names", "marker-types", "printer-info",
        "printer-state", "printer-state-message", "printer-state-reasons",
    };
    CHECK(standin->requested_count == 7);
    for (int i = 0; i < 7; i++) CHECK(strcmp(standin->requested[i], expected[i]) == 0);
    standin_unlock(standin);

    /* Reasons: lower case, "none" and repeats dropped, at most 16, and
     * keywords too long for the table skipped. */
    char reasons[2048] = "Media-Jam-Error,media-jam-error,none,";
    for (int i = 0; i < 18; i++) {
        char reason[32];
        snprintf(reason, sizeof(reason), "r%02d-report,", i);
        strcat(reasons, reason);
    }
    set_printer(standin, TEST_QUEUE, 5, reasons, "Line\none\ttwo", -99);
    CHECK(p1600_notify_snapshot(http, TEST_QUEUE, &snapshot) == P1600_NOTIFY_OK);
    CHECK(snapshot.state == 5 && snapshot.reason_count == P1600_NOTIFY_MAX_REASONS);
    CHECK(strcmp(snapshot.reasons[0], "media-jam-error") == 0 && strcmp(snapshot.reasons[1], "r00-report") == 0);
    /* libcups (ippReadIO) ends a text value at its first control character. */
    CHECK(strcmp(snapshot.message, "Line") == 0);
    char long_reason[80];
    memset(long_reason, 'x', 70);
    strcpy(long_reason + 70, "-error");
    char list[160];
    snprintf(list, sizeof(list), "%s,cover-open-error", long_reason);
    set_printer(standin, TEST_QUEUE, 4, list, P1600_OUT_OF_PAPER, -99);
    CHECK(p1600_notify_snapshot(http, TEST_QUEUE, &snapshot) == P1600_NOTIFY_OK);
    CHECK(snapshot.reason_count == 1 && strcmp(snapshot.reasons[0], "cover-open-error") == 0);
    CHECK(strcmp(snapshot.message, P1600_OUT_OF_PAPER) == 0);

    /* The toner marker among several, and levels outside 0-100. */
    static const struct {
        int count;
        int levels[3];
        const char *types[3];
        const char *names[3];
        int toner;
    } markers[] = {
        {2, {80, 23}, {"waste-toner", "toner"}, {"Waste", "Black"}, 23},
        {2, {90, 33}, {"other", "other"}, {"Drum", "Black Cartridge"}, 33},
        {2, {90, 33}, {"other", "other"}, {"Drum", "Fuser"}, 90},
        {1, {77}, {"other"}, {"Supply"}, 77},
        {1, {64}, {"ink"}, {"Black"}, 64},
        {1, {-3}, {"toner"}, {"Black"}, -1},
        {1, {-1}, {"toner"}, {"Black"}, -1},
        {1, {101}, {"toner"}, {"Black"}, -1},
        {0, {0}, {""}, {""}, -1},
    };
    for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); i++) {
        standin_lock(standin);
        FakePrinter *printer = &standin->printers[0];
        printer->marker_count = markers[i].count;
        for (int m = 0; m < markers[i].count; m++) {
            printer->levels[m] = markers[i].levels[m];
            snprintf(printer->types[m], sizeof(printer->types[m]), "%s", markers[i].types[m]);
            snprintf(printer->names[m], sizeof(printer->names[m]), "%s", markers[i].names[m]);
        }
        standin_unlock(standin);
        CHECK(p1600_notify_snapshot(http, TEST_QUEUE, &snapshot) == P1600_NOTIFY_OK);
        CHECK(snapshot.toner == markers[i].toner);
    }

    /* A UTF-8 queue name travels percent-encoded. */
    set_printer(standin, "Ofis_Yazıcı", 3, "", "", 55);
    CHECK(p1600_notify_snapshot(http, "Ofis_Yazıcı", &snapshot) == P1600_NOTIFY_OK && snapshot.toner == 55);
    standin_lock(standin);
    CHECK(strcmp(standin->printer_uri, "ipp://localhost/printers/Ofis_Yaz%C4%B1c%C4%B1") == 0);
    standin_unlock(standin);

    /* Errors. */
    int before = operation_count(standin, IPP_OP_GET_PRINTER_ATTRIBUTES);
    CHECK(p1600_notify_snapshot(http, "P1600DN_No_Such_Queue", &snapshot) == P1600_NOTIFY_NOT_FOUND);
    CHECK(!snapshot.has_data && snapshot.toner == -1);
    CHECK(p1600_notify_snapshot(http, "", &snapshot) == P1600_NOTIFY_FAILED);
    CHECK(p1600_notify_snapshot(http, "a/b", &snapshot) == P1600_NOTIFY_FAILED);
    CHECK(p1600_notify_snapshot(http, "a b", &snapshot) == P1600_NOTIFY_FAILED);
    CHECK(p1600_notify_snapshot(http, NULL, &snapshot) == P1600_NOTIFY_FAILED);
    CHECK(p1600_notify_snapshot(NULL, TEST_QUEUE, &snapshot) == P1600_NOTIFY_FAILED);
    CHECK(p1600_notify_snapshot(http, TEST_QUEUE, NULL) == P1600_NOTIFY_FAILED);
    CHECK(operation_count(standin, IPP_OP_GET_PRINTER_ATTRIBUTES) == before + 1);
    httpClose(http);
    set_printer(standin, TEST_QUEUE, 3, "", "", 40);
    puts("[PASS] Get-Printer-Attributes: the 7 attributes, reasons normalized, message, toner marker choice, errors");
}

typedef struct {
    Standin *standin;
    int delay_ms;
} LaterEvent;

static void *push_later_main(void *argument) {
    LaterEvent *later = argument;
    pause_ms(later->delay_ms);
    push_event(later->standin, TEST_QUEUE, "printer-state-changed", 0);
    return NULL;
}

static void set_option(Standin *standin, int *option, int value) {
    standin_lock(standin);
    *option = value;
    standin_unlock(standin);
}

static void set_flag(Standin *standin, bool *flag, bool value) {
    standin_lock(standin);
    *flag = value;
    standin_unlock(standin);
}

static void test_subscriptions(Standin *standin) {
    http_t *http = p1600_notify_connect(5000);
    CHECK(http);
    P1600NotifyEvent events[8];
    int id = 0, sequence = 1;

    set_option(standin, &standin->max_lease, 600);
    CHECK(p1600_notify_subscribe(http, TEST_QUEUE, 3600, &id) == 600 && id > 0);
    standin_lock(standin);
    CHECK(strcmp(standin->pull_method, "ippget") == 0 && standin->subscribe_lease == 3600);
    CHECK(standin->notify_event_count == 3);
    CHECK(strcmp(standin->notify_events[0], "printer-state-changed") == 0);
    CHECK(strcmp(standin->notify_events[1], "printer-stopped") == 0);
    CHECK(strcmp(standin->notify_events[2], "job-completed") == 0);
    CHECK(strcmp(standin->printer_uri, "ipp://localhost/printers/" TEST_QUEUE) == 0);
    standin->max_lease = 0;
    standin_unlock(standin);

    /* No events: the long poll is asked again until the time is up. */
    int polls = operation_count(standin, IPP_OP_GET_NOTIFICATIONS);
    double started = now_seconds();
    CHECK(p1600_notify_wait(http, id, &sequence, 1, events, 8) == 0);
    double elapsed = now_seconds() - started;
    CHECK(elapsed >= 0.9 && elapsed < 3.0 && sequence == 1);
    CHECK(operation_count(standin, IPP_OP_GET_NOTIFICATIONS) >= polls + 2);
    standin_lock(standin);
    CHECK(standin->asked_wait && standin->asked_sequence == 1);
    CHECK(strcmp(standin->printer_uri, "ipp://localhost/") == 0);
    standin_unlock(standin);

    /* Events in order, sequence numbers advanced, more than `max` kept. */
    set_printer(standin, TEST_QUEUE, 4, "media-empty-error", P1600_OUT_OF_PAPER, -99);
    CHECK(p1600_notify_wait(http, id, &sequence, 5, events, 8) == 1);
    CHECK(events[0].kind == P1600_EVENT_PRINTER_STATE_CHANGED && events[0].sequence == 1 && sequence == 2);
    CHECK(strcmp(events[0].queue, TEST_QUEUE) == 0 && events[0].job_id == 0);
    push_event(standin, TEST_QUEUE, "printer-stopped", 0);
    push_event(standin, TEST_QUEUE, "job-completed", 7);
    push_event(standin, TEST_QUEUE, "printer-config-changed", 0);
    push_event(standin, TEST_QUEUE, "printer-state-changed", 0);
    CHECK(p1600_notify_wait(http, id, &sequence, 5, events, 2) == 2);
    CHECK(events[0].kind == P1600_EVENT_PRINTER_STOPPED && events[0].sequence == 2);
    CHECK(events[1].kind == P1600_EVENT_JOB_COMPLETED && events[1].job_id == 7 && events[1].sequence == 3);
    CHECK(strcmp(events[1].queue, TEST_QUEUE) == 0);  /* from notify-printer-uri */
    CHECK(sequence == 4);
    CHECK(p1600_notify_wait(http, id, &sequence, 5, events, 8) == 2);
    CHECK(events[0].kind == P1600_EVENT_OTHER && events[1].kind == P1600_EVENT_PRINTER_STATE_CHANGED && sequence == 6);
    standin_lock(standin);
    CHECK(standin->asked_sequence == 4);
    standin_unlock(standin);

    /* A repeated event is dropped. */
    set_flag(standin, &standin->replay_one, true);
    push_event(standin, TEST_QUEUE, "printer-stopped", 0);
    CHECK(p1600_notify_wait(http, id, &sequence, 5, events, 8) == 1);
    CHECK(events[0].sequence == 6 && events[0].kind == P1600_EVENT_PRINTER_STOPPED && sequence == 7);
    set_flag(standin, &standin->replay_one, false);

    /* An event during the long poll ends it at once. */
    set_option(standin, &standin->hold_ms, 5000);
    LaterEvent later = {standin, 200};
    pthread_t pusher;
    CHECK(pthread_create(&pusher, NULL, push_later_main, &later) == 0);
    started = now_seconds();
    CHECK(p1600_notify_wait(http, id, &sequence, 10, events, 8) == 1);
    elapsed = now_seconds() - started;
    pthread_join(pusher, NULL);
    CHECK(elapsed >= 0.15 && elapsed < 2.0 && sequence == 8);
    set_option(standin, &standin->hold_ms, 300);

    /* Renewal: cupsd may grant less than asked. */
    set_option(standin, &standin->max_lease, 900);
    CHECK(p1600_notify_renew(http, id, 3600) == 900);
    standin_lock(standin);
    CHECK(standin->renew_lease == 3600 && standin->operations[IPP_OP_RENEW_SUBSCRIPTION] == 1);
    standin->max_lease = 0;
    standin_unlock(standin);
    CHECK(p1600_notify_renew(http, id, 1800) == 1800);
    CHECK(p1600_notify_renew(http, 999, 1800) == P1600_NOTIFY_NOT_FOUND);
    CHECK(p1600_notify_renew(http, 0, 1800) == P1600_NOTIFY_FAILED && p1600_notify_renew(NULL, id, 60) == P1600_NOTIFY_FAILED);
    CHECK(p1600_notify_renew(http, id, -1) == P1600_NOTIFY_FAILED);

    /* The server ends the subscription. */
    set_flag(standin, &standin->events_complete, true);
    CHECK(p1600_notify_wait(http, id, &sequence, 1, events, 8) == P1600_NOTIFY_NOT_FOUND);
    set_flag(standin, &standin->events_complete, false);

    /* Cancel, then everything about it is gone. */
    CHECK(p1600_notify_cancel(http, id) == P1600_NOTIFY_OK);
    standin_lock(standin);
    CHECK(standin->operations[IPP_OP_CANCEL_SUBSCRIPTION] == 1);
    standin_unlock(standin);
    CHECK(p1600_notify_wait(http, id, &sequence, 1, events, 8) == P1600_NOTIFY_NOT_FOUND);
    CHECK(p1600_notify_renew(http, id, 60) == P1600_NOTIFY_NOT_FOUND);
    CHECK(p1600_notify_cancel(http, id) == P1600_NOTIFY_NOT_FOUND);
    CHECK(p1600_notify_cancel(http, 0) == P1600_NOTIFY_FAILED && p1600_notify_cancel(NULL, id) == P1600_NOTIFY_FAILED);

    /* Unknown queue, refused subscription, invalid arguments. */
    int other = 5;
    CHECK(p1600_notify_subscribe(http, "P1600DN_No_Such_Queue", 60, &other) == P1600_NOTIFY_NOT_FOUND && other == 0);
    set_flag(standin, &standin->refuse_subscriptions, true);
    other = 5;
    CHECK(p1600_notify_subscribe(http, TEST_QUEUE, 60, &other) == P1600_NOTIFY_FAILED && other == 0);
    set_flag(standin, &standin->refuse_subscriptions, false);
    CHECK(p1600_notify_subscribe(http, TEST_QUEUE, -1, &other) == P1600_NOTIFY_FAILED);
    CHECK(p1600_notify_subscribe(http, TEST_QUEUE, 60, NULL) == P1600_NOTIFY_FAILED);
    CHECK(p1600_notify_subscribe(http, "", 60, &other) == P1600_NOTIFY_FAILED);
    CHECK(p1600_notify_subscribe(NULL, TEST_QUEUE, 60, &other) == P1600_NOTIFY_FAILED);
    CHECK(p1600_notify_wait(http, 0, &sequence, 1, events, 8) == P1600_NOTIFY_FAILED);
    CHECK(p1600_notify_wait(http, 5, NULL, 1, events, 8) == P1600_NOTIFY_FAILED);
    CHECK(p1600_notify_wait(http, 5, &sequence, 1, NULL, 8) == P1600_NOTIFY_FAILED);
    CHECK(p1600_notify_wait(NULL, 5, &sequence, 1, events, 8) == P1600_NOTIFY_FAILED);

    /* A lease of 0 never expires; sequence numbers start at 1. */
    CHECK(p1600_notify_subscribe(http, TEST_QUEUE, 0, &other) == 0 && other > 0 && other != id);
    sequence = 0;
    CHECK(p1600_notify_wait(http, other, &sequence, 0, events, 8) == 0 && sequence == 1);
    CHECK(p1600_notify_cancel(http, other) == P1600_NOTIFY_OK);
    httpClose(http);
    puts("[PASS] Subscriptions: ippget with the 3 events, long poll, sequence numbers, repeats, renewal, cancel, errors");
}

static void test_cupsd_polling(Standin *standin) {
    http_t *http = p1600_notify_connect(5000);
    CHECK(http);
    P1600NotifyEvent events[8];
    int id = 0, sequence = 1;
    CHECK(p1600_notify_subscribe(http, TEST_QUEUE, 300, &id) == 300);
    standin_lock(standin);
    standin->cupsd_mode = true;
    standin->interval = 1;
    standin_unlock(standin);

    /* cupsd answers at once and names an interval: asked every second. */
    int polls = operation_count(standin, IPP_OP_GET_NOTIFICATIONS);
    double started = now_seconds();
    CHECK(p1600_notify_wait(http, id, &sequence, 3, events, 8) == 0);
    double elapsed = now_seconds() - started;
    int asked = operation_count(standin, IPP_OP_GET_NOTIFICATIONS) - polls;
    CHECK(elapsed >= 2.9 && elapsed < 4.5 && asked >= 2 && asked <= 4);

    /* A long interval: one question, then the rest of the time asleep. */
    set_option(standin, &standin->interval, 60);
    polls = operation_count(standin, IPP_OP_GET_NOTIFICATIONS);
    started = now_seconds();
    CHECK(p1600_notify_wait(http, id, &sequence, 2, events, 8) == 0);
    elapsed = now_seconds() - started;
    CHECK(elapsed >= 1.9 && elapsed < 3.0 && operation_count(standin, IPP_OP_GET_NOTIFICATIONS) == polls + 1);

    /* Pending events come back at once; an interval of 0 does not spin. */
    set_printer(standin, TEST_QUEUE, 3, "", "", -99);
    started = now_seconds();
    CHECK(p1600_notify_wait(http, id, &sequence, 5, events, 8) == 1 && now_seconds() - started < 1.0);
    set_option(standin, &standin->interval, 0);
    polls = operation_count(standin, IPP_OP_GET_NOTIFICATIONS);
    CHECK(p1600_notify_wait(http, id, &sequence, 2, events, 8) == 0);
    CHECK(operation_count(standin, IPP_OP_GET_NOTIFICATIONS) - polls <= 3);
    set_flag(standin, &standin->cupsd_mode, false);
    CHECK(p1600_notify_cancel(http, id) == P1600_NOTIFY_OK);
    httpClose(http);
    puts("[PASS] cupsd-style Get-Notifications: notify-get-interval honoured within the wait, no busy loop");
}

static void test_request_levels(Standin *standin) {
    /* This thread's default connection must be the stand-in, never cupsd. */
    CHECK(strcmp(cupsServer(), "127.0.0.1") == 0 && ippPort() == standin->port);
    int job = p1600_notify_request_levels(TEST_QUEUE);
    CHECK(job > 0);
    standin_lock(standin);
    CHECK(standin->operations[IPP_OP_CREATE_JOB] == 1 && standin->operations[IPP_OP_SEND_DOCUMENT] == 1);
    CHECK(strcmp(standin->job_name, "ReportLevels") == 0);
    CHECK(strcmp(standin->document_format, "application/vnd.cups-command") == 0);
    CHECK(strcmp(standin->document_format, CUPS_FORMAT_COMMAND) == 0);
    CHECK(standin->last_document && standin->document_job == job);
    CHECK(standin->document_length == 27);
    CHECK(memcmp(standin->document, "#CUPS-COMMAND\nReportLevels\n", 27) == 0);
    CHECK(strncmp(standin->printer_uri, "ipp://localhost", 15) == 0 && strstr(standin->printer_uri, "/printers/" TEST_QUEUE));
    standin_unlock(standin);

    CHECK(p1600_notify_request_levels("P1600DN_No_Such_Queue") == P1600_NOTIFY_NOT_FOUND);
    CHECK(p1600_notify_request_levels("") == P1600_NOTIFY_FAILED && p1600_notify_request_levels(NULL) == P1600_NOTIFY_FAILED);
    CHECK(p1600_notify_request_levels("a/b") == P1600_NOTIFY_FAILED);
    standin_lock(standin);
    CHECK(standin->operations[IPP_OP_SEND_DOCUMENT] == 1);
    standin_unlock(standin);
    puts("[PASS] ReportLevels: Create-Job + Send-Document, application/vnd.cups-command, 27-byte command file, errors");
}

#define WATCHERS 4

typedef struct {
    Standin *standin;
    atomic_int *subscribed;
    int received;
    int job;
} Watcher;

/* What each agent thread does: its own connection, a subscription, a long
 * poll, snapshots and a ReportLevels job. */
static void *watcher_main(void *argument) {
    Watcher *watcher = argument;
    CHECK(strcmp(cupsServer(), "127.0.0.1") == 0 && ippPort() == watcher->standin->port);
    http_t *http = p1600_notify_connect(5000);
    CHECK(http);
    P1600PrinterSnapshot snapshot;
    for (int i = 0; i < 10; i++) CHECK(p1600_notify_snapshot(http, TEST_QUEUE, &snapshot) == P1600_NOTIFY_OK);
    int id = 0, sequence = 1;
    CHECK(p1600_notify_subscribe(http, TEST_QUEUE, 300, &id) > 0);
    atomic_fetch_add(watcher->subscribed, 1);
    P1600NotifyEvent events[4];
    for (int round = 0; round < 40 && watcher->received == 0; round++) {
        int count = p1600_notify_wait(http, id, &sequence, 1, events, 4);
        CHECK(count >= 0);
        watcher->received += count;
    }
    CHECK(p1600_notify_snapshot(http, TEST_QUEUE, &snapshot) == P1600_NOTIFY_OK && snapshot.toner == 31);
    CHECK(p1600_notify_cancel(http, id) == P1600_NOTIFY_OK);
    httpClose(http);
    watcher->job = p1600_notify_request_levels(TEST_QUEUE);
    return NULL;
}

static void test_threads(Standin *standin) {
    atomic_int count;
    atomic_init(&count, 0);
    Watcher watchers[WATCHERS];
    pthread_t threads[WATCHERS];
    for (int i = 0; i < WATCHERS; i++) {
        watchers[i] = (Watcher){standin, &count, 0, 0};
        CHECK(pthread_create(&threads[i], NULL, watcher_main, &watchers[i]) == 0);
    }
    for (int wait = 0; wait < 500 && atomic_load(&count) < WATCHERS; wait++) pause_ms(10);
    CHECK(atomic_load(&count) == WATCHERS);
    set_printer(standin, TEST_QUEUE, 4, "", "", 31);
    for (int i = 0; i < WATCHERS; i++) {
        pthread_join(threads[i], NULL);
        CHECK(watchers[i].received >= 1 && watchers[i].job > 0);
    }
    puts("[PASS] Threads: 4 agent threads with their own connections and the stand-in through CUPS_SERVER");
}

static void test_server_down(Standin *standin) {
    http_t *http = p1600_notify_connect(5000);
    CHECK(http);
    P1600PrinterSnapshot snapshot;
    P1600NotifyEvent events[4];
    int id = 0, other = 0, sequence = 1;
    CHECK(p1600_notify_subscribe(http, TEST_QUEUE, 300, &id) == 300);
    CHECK(p1600_notify_snapshot(http, TEST_QUEUE, &snapshot) == P1600_NOTIFY_OK);

    const int port = standin->port;
    standin_stop(standin);
    double started = now_seconds();
    CHECK(p1600_notify_snapshot(http, TEST_QUEUE, &snapshot) == P1600_NOTIFY_UNREACHABLE && !snapshot.has_data);
    CHECK(p1600_notify_wait(http, id, &sequence, 1, events, 4) == P1600_NOTIFY_UNREACHABLE);
    CHECK(p1600_notify_subscribe(http, TEST_QUEUE, 60, &other) == P1600_NOTIFY_UNREACHABLE);
    CHECK(p1600_notify_renew(http, id, 60) == P1600_NOTIFY_UNREACHABLE);
    CHECK(p1600_notify_cancel(http, id) == P1600_NOTIFY_UNREACHABLE);
    CHECK(p1600_notify_request_levels(TEST_QUEUE) == P1600_NOTIFY_UNREACHABLE);
    CHECK(p1600_notify_connect(1000) == NULL);
    CHECK(now_seconds() - started < 10.0);
    httpClose(http);

    /* cupsd is back (a new one: the subscription is gone). */
    standin_start(standin, port);
    set_printer(standin, TEST_QUEUE, 3, "", "", 22);
    http = p1600_notify_connect(5000);
    CHECK(http);
    CHECK(p1600_notify_snapshot(http, TEST_QUEUE, &snapshot) == P1600_NOTIFY_OK && snapshot.toner == 22);
    CHECK(p1600_notify_wait(http, id, &sequence, 1, events, 4) == P1600_NOTIFY_NOT_FOUND);
    CHECK(p1600_notify_subscribe(http, TEST_QUEUE, 300, &id) == 300 && id > 0);
    sequence = 1;
    set_printer(standin, TEST_QUEUE, 3, "media-jam-error", "", -99);
    CHECK(p1600_notify_wait(http, id, &sequence, 5, events, 4) == 1 && sequence == 2);
    CHECK(p1600_notify_request_levels(TEST_QUEUE) > 0);
    CHECK(p1600_notify_cancel(http, id) == P1600_NOTIFY_OK);
    httpClose(http);
    puts("[PASS] cupsd down: every call reports unreachable at once; after a restart the subscription is renewed");
}

/* --- stand-in for the app test ---------------------------------------------- */

/* The driver's command filter answers ReportLevels: the cartridge is empty. */
static void report_levels_hook(Standin *standin, int printer) {
    set_markers_locked(&standin->printers[printer], 0);
    push_event_locked(standin, printer, "job-completed", standin->next_job_id);
    push_event_locked(standin, printer, "printer-state-changed", 0);
}

static int snapshot_count(Standin *standin) {
    standin_lock(standin);
    int count = standin->snapshots;
    standin_unlock(standin);
    return count;
}

static bool wait_for_count(Standin *standin, ipp_op_t operation, int count, int timeout_seconds) {
    double deadline = now_seconds() + timeout_seconds;
    while (operation_count(standin, operation) < count) {
        if (now_seconds() > deadline) return false;
        pause_ms(20);
    }
    return true;
}

static bool wait_for_snapshots(Standin *standin, int count, int timeout_seconds) {
    double deadline = now_seconds() + timeout_seconds;
    while (snapshot_count(standin) < count) {
        if (now_seconds() > deadline) return false;
        pause_ms(20);
    }
    return true;
}

/* Changes the printer and waits until the agent has looked at it again. */
static bool step(Standin *standin, const char *what, int state, const char *reasons, const char *message, int toner) {
    int seen = snapshot_count(standin);
    set_printer(standin, TEST_QUEUE, state, reasons, message, toner);
    bool ok = wait_for_snapshots(standin, seen + 1, 30);
    fprintf(stderr, "serve: %s%s\n", what, ok ? "" : " (the agent did not look)");
    pause_ms(400);
    return ok;
}

static int serve(const char *port_file, const char *scenario) {
    static Standin standin;
    const bool main_scenario = strcmp(scenario, "main") == 0;
    if (!main_scenario && strcmp(scenario, "persist") != 0) {
        fprintf(stderr, "Unknown scenario \"%s\"\n", scenario);
        return 2;
    }
    standin_start(&standin, 0);
    if (main_scenario) {
        set_printer(&standin, TEST_QUEUE, 3, "", "", 40);
    } else {
        /* After the restart the stack still waits for the Go button (reported
         * before the restart), and the toner level is not known yet. */
        set_printer(&standin, TEST_QUEUE, 4, "media-needed-error", P1600_MANUAL_DUPLEX, -100);
    }
    standin_lock(&standin);
    snprintf(standin.printers[0].info, sizeof(standin.printers[0].info), "Office P1606dn");
    if (main_scenario) standin.on_command = report_levels_hook;
    standin.hold_ms = 5000;  /* a real long poll */
    standin_unlock(&standin);

    char partial[1024];
    snprintf(partial, sizeof(partial), "%s.partial", port_file);
    FILE *file = fopen(partial, "w");
    CHECK(file);
    fprintf(file, "%d\n", standin.port);
    fclose(file);
    CHECK(rename(partial, port_file) == 0);

    /* The agent subscribes, then takes its first snapshot (priming). */
    bool ok = wait_for_count(&standin, IPP_OP_CREATE_PRINTER_SUBSCRIPTIONS, 1, 60) && wait_for_snapshots(&standin, 1, 30);
    pause_ms(500);
    if (main_scenario) {
        ok = ok && step(&standin, "out of paper", 4, "media-empty-error", P1600_OUT_OF_PAPER, -99);
        ok = ok && step(&standin, "out of paper again", 4, "media-empty-error", P1600_OUT_OF_PAPER, -99);
        ok = ok && step(&standin, "paper loaded", 3, "", "", -99);
        ok = ok && step(&standin, "toner 12%", -1, NULL, NULL, 12);
        ok = ok && step(&standin, "toner 12% again", -1, NULL, NULL, 12);
        ok = ok && step(&standin, "toner 4%", -1, NULL, NULL, 4);
        /* A job ends: the agent asks for the toner level after 20 s. */
        if (ok) {
            push_event(&standin, TEST_QUEUE, "job-completed", 40);
            ok = wait_for_count(&standin, IPP_OP_SEND_DOCUMENT, 1, 60);
            int seen = snapshot_count(&standin);
            ok = ok && wait_for_snapshots(&standin, seen + 1, 30);
            fprintf(stderr, "serve: job completed, ReportLevels%s\n", ok ? "" : " (not requested)");
            pause_ms(400);
        }
        ok = ok && step(&standin, "manual duplex", 4, "media-needed-error", P1600_MANUAL_DUPLEX, -99);
    } else {
        ok = ok && step(&standin, "toner 0% (reported before the restart)", -1, NULL, NULL, 0);
        ok = ok && step(&standin, "paper jam", 4, "media-needed-error,media-jam-error", "", -99);
        /* A backend connecting for a moment is no news; a printer that stays
         * unreachable past the settle time is. */
        ok = ok && step(&standin, "connecting", 4, "connecting-to-device", "Connecting to printer.", -99);
        pause_ms(2000);
        ok = ok && step(&standin, "connected", 4, "", "", -99);
        ok = ok && step(&standin, "unreachable", 4, "connecting-to-device", "The printer is unreachable at this time.", -99);
    }
    /* The agent cancels its subscription when it exits. */
    bool cancelled = ok && wait_for_count(&standin, IPP_OP_CANCEL_SUBSCRIPTION, 1, 30);
    standin_lock(&standin);
    bool levels = standin.document_length == 27 && memcmp(standin.document, P1600_NOTIFY_REPORT_LEVELS, 27) == 0 &&
                  strcmp(standin.document_format, "application/vnd.cups-command") == 0;
    int jobs = standin.operations[IPP_OP_CREATE_JOB];
    standin_unlock(&standin);
    printf("steps=%s cancel=%s reportlevels=%s jobs=%d\n", ok ? "ok" : "failed", cancelled ? "ok" : "missing",
           levels ? "ok" : "none", jobs);
    fflush(stdout);
    standin_stop(&standin);
    return ok && cancelled && (!main_scenario || (levels && jobs == 1)) ? 0 : 1;
}

/* One read-only Get-Printer-Attributes against this Mac's cupsd. */
static int real_snapshot(const char *queue) {
    http_t *http = p1600_notify_connect(5000);
    if (!http) {
        fprintf(stderr, "cupsd is not reachable at %s\n", cupsServer());
        return 1;
    }
    P1600PrinterSnapshot snapshot;
    int status = p1600_notify_snapshot(http, queue, &snapshot);
    httpClose(http);
    printf("server=%s status=%d has_data=%d state=%d toner=%d info=\"%s\" message=\"%s\" reasons=", cupsServer(),
           status, snapshot.has_data, snapshot.state, snapshot.toner, snapshot.info, snapshot.message);
    for (int i = 0; i < snapshot.reason_count; i++) printf("%s%s", i ? "," : "", snapshot.reasons[i]);
    printf("\n");
    return status == P1600_NOTIFY_OK ? 0 : 1;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    if (argc == 4 && strcmp(argv[1], "--serve") == 0) return serve(argv[2], argv[3]);
    if (argc == 3 && strcmp(argv[1], "--real-snapshot") == 0) return real_snapshot(argv[2]);
    if (argc != 1) {
        fprintf(stderr, "Usage: %s [--serve <port file> <main|persist> | --real-snapshot <queue>]\n", argv[0]);
        return 2;
    }
    test_reasons();
    test_attention();
    test_toner();
    test_memory();

    static Standin standin;
    standin_start(&standin, 0);
    char server[64];
    snprintf(server, sizeof(server), "127.0.0.1:%d", standin.port);
    p1600_notify_set_server(server);
    CHECK(strcmp(cupsServer(), "127.0.0.1") == 0 && ippPort() == standin.port);
    const char *environment = getenv("CUPS_SERVER");
    CHECK(environment && strcmp(environment, server) == 0);
    p1600_notify_set_server(NULL);
    p1600_notify_set_server("");
    environment = getenv("CUPS_SERVER");
    CHECK(environment && strcmp(environment, server) == 0);

    test_snapshot(&standin);
    test_subscriptions(&standin);
    test_cupsd_polling(&standin);
    test_request_levels(&standin);
    test_threads(&standin);
    test_server_down(&standin);
    standin_stop(&standin);
    puts("[PASS] Notifier suite");
    return 0;
}
