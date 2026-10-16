/*
 * p1600dn-utility - printer utility for the HP LaserJet Professional P1600dn
 * series from the command line: EcoSMART sleep and auto-off, Quiet Mode,
 * information pages, the cleaning page and the printer's web pages, with
 * PJL (src/p1600_utility.c), and the driver's diagnostic page
 * (src/p1600_diagnostic.c), printed through the CUPS queue. The P1600dn
 * Utility app is the window for the same functions.
 *
 * Komut satırından P1600dn Utility (HP LaserJet Professional P1600dn serisi).
 */
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "p1600_diagnostic.h"
#include "p1600_ews_proxy.h"
#include "p1600_queues.h"
#include "p1600_utility.h"

static bool english = false;

static const char *text(const char *turkish, const char *english_text) {
    return english ? english_text : turkish;
}

static void usage(FILE *out) {
    fputs(text("Kullanım: p1600dn-utility [--en|--tr] [--queue KUYRUK | --device-uri URI] [--dry-run]\n"
               "                          [--sides one|long|short] [--media KAĞIT] [--output DOSYA.pdf] KOMUT\n"
               "Komutlar:\n"
               "  list                                  P1600dn serisi kuyrukları\n"
               "  status                                Uyku, otomatik kapanma ve sessiz mod ayarları\n"
               "  sleep 1|5|15|30|60|120                Şu kadar dakika sonra uyku (EcoSMART)\n"
               "  auto-off never|30|60|120|240|480|1440 Şu kadar dakika sonra otomatik kapanma\n"
               "  quiet on|off                          Sessiz mod\n"
               "  page demo|configuration|supplies [DİL] Bilgi sayfası yazdır (DİL: tr, de, en ...)\n"
               "  page diagnostic [tr|en]               Sürücünün teşhis sayfası (alt kenar, gri tonlar, çift taraf)\n"
               "  clean                                 Temizleme sayfası (1. tepsiye asetat/fotokopi kağıdı)\n"
               "  web                                   Yazıcının web sayfaları (USB'de yerel vekil üzerinden)\n",
               "Usage: p1600dn-utility [--en|--tr] [--queue QUEUE | --device-uri URI] [--dry-run]\n"
               "                          [--sides one|long|short] [--media PAPER] [--output FILE.pdf] COMMAND\n"
               "Commands:\n"
               "  list                                  P1600dn series queues\n"
               "  status                                Sleep, auto-off and Quiet Mode settings\n"
               "  sleep 1|5|15|30|60|120                Sleep after this many minutes (EcoSMART)\n"
               "  auto-off never|30|60|120|240|480|1440 Turn off after this many minutes\n"
               "  quiet on|off                          Quiet Mode\n"
               "  page demo|configuration|supplies [LANG] Print an information page (LANG: en, de, tr ...)\n"
               "  page diagnostic [tr|en]               The driver's diagnostic page (bottom edge, grays, 2-sided)\n"
               "  clean                                 Cleaning page (transparency/copier paper in Tray 1)\n"
               "  web                                   The printer's web pages (through a local proxy on USB)\n"),
          out);
}

static int number(const char *value, int *out) {
    if (!value || !*value) return -1;
    char *end = NULL;
    errno = 0;
    long parsed = strtol(value, &end, 10);
    if (*end != '\0' || errno != 0 || parsed < 0 || parsed > 100000) return -1;
    *out = (int)parsed;
    return 0;
}

/* Prints bytes with ESC and LF visible, for --dry-run. */
static void show_bytes(const char *data, size_t length) {
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)data[i];
        if (c == 0x1b) fputs("<ESC>", stdout);
        else if (c == '\n') fputs("<LF>\n", stdout);
        else putchar(c);
    }
    if (length > 0 && data[length - 1] != '\n') putchar('\n');
}

static int list_queues(void) {
    P1600Queue queues[32];
    int count = p1600_queue_list(queues, 32);
    if (count <= 0) {
        puts(text("P1600dn serisi yazıcı kuyruğu bulunamadı.", "No P1600dn series printer queue found."));
        return count < 0 ? 1 : 0;
    }
    for (int i = 0; i < count && i < 32; i++) {
        printf("%s%s\t%s\t%s\n", queues[i].name, queues[i].is_default ? " *" : "", queues[i].info, queues[i].device_uri);
    }
    return 0;
}

static void show_minutes(const char *label, int minutes, bool auto_off) {
    printf("%-22s ", label);
    if (minutes < 0) puts(text("okunamadı", "could not be read"));
    else if (auto_off && minutes == 0) puts(text("hiçbir zaman", "never"));
    else printf("%d %s\n", minutes, text("dakika sonra", "minutes"));
}

static int show_status(const char *device_uri) {
    P1600PrinterSettings settings;
    char error[256] = "";
    if (p1600_utility_read_settings(device_uri, &settings, error, sizeof(error)) < 0) {
        fprintf(stderr, "%s: %s\n", text("Yazıcıya ulaşılamadı", "The printer could not be reached"), error);
        return 1;
    }
    show_minutes(text("Uyku (EcoSMART):", "Sleep after:"), settings.sleep_minutes, false);
    show_minutes(text("Otomatik kapanma:", "Auto Off after:"), settings.auto_off_minutes, true);
    printf("%-22s %s\n", text("Sessiz mod:", "Quiet Mode:"),
           settings.quiet_mode < 0 ? text("okunamadı", "could not be read")
           : settings.quiet_mode   ? text("açık", "on")
                                   : text("kapalı", "off"));
    return 0;
}

static int open_web(const char *device_uri, bool dry_run) {
    char url[512], error[256] = "";
    int network = p1600_utility_web_address(device_uri, url, sizeof(url), error, sizeof(error));
    if (network < 0) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    if (network == 0) {
        puts(url);
        if (!dry_run) execl("/usr/bin/open", "open", url, (char *)NULL);
        return 0;
    }
    if (dry_run) {
        puts("http://127.0.0.1:<port>" P1600_EWS_START_PAGE);
        return 0;
    }
    /* USB: a local web page proxy until the pages are idle for 3 minutes. */
    P1600EwsProxy *proxy = p1600_ews_proxy_start_usb(device_uri, P1600_EWS_IDLE_SECONDS, url, sizeof(url), error, sizeof(error));
    if (!proxy) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    printf("%s\n%s\n", url,
           text("Yazıcının sayfaları 3 dakika kullanılmayınca kapanır (Ctrl-C ile hemen).",
                "The printer's pages close after 3 idle minutes (Ctrl-C to stop now)."));
    fflush(stdout);
    pid_t child = fork();
    if (child == 0) {
        execl("/usr/bin/open", "open", url, (char *)NULL);
        _exit(127);
    }
    p1600_ews_proxy_wait(proxy);
    p1600_ews_proxy_stop(proxy);
    return 0;
}

/* The driver's diagnostic page: printed on the queue through CUPS (the
 * whole driver path, page guard included), or written to `output`. */
static int diagnostic_page(const char *queue_name, const char *language, const char *sides, const char *media,
                           const char *output) {
    P1600DiagnosticInfo info;
    char error[256] = "";
    p1600_diagnostic_defaults(&info, language);
    if (sides && p1600_sides_parse(sides, &info.sides) != 0) {
        usage(stderr);
        return 2;
    }
    if ((queue_name || !output) && p1600_diagnostic_info_for_queue(queue_name ? queue_name : "", &info, error,
                                                                sizeof(error)) != 0) {
        /* No queue named: the default P1600dn queue. */
        P1600Queue queue;
        if (queue_name || p1600_queue_find(NULL, &queue, error, sizeof(error)) != 0 ||
            p1600_diagnostic_info_for_queue(queue.name, &info, error, sizeof(error)) != 0) {
            fprintf(stderr, "%s\n", error);
            return 1;
        }
    }
    if (media && p1600_diagnostic_set_paper(&info, media) != 0) {
        fprintf(stderr, "%s: %s\n", text("Bu kağıt boyutu desteklenmiyor", "Unsupported paper size"), media);
        return 2;
    }
    if (output) {
        if (p1600_diagnostic_write_pdf(output, &info, error, sizeof(error)) != 0) {
            fprintf(stderr, "%s\n", error);
            return 1;
        }
        printf("%s\n", output);
        return 0;
    }
    int job = 0;
    if (p1600_diagnostic_print(&info, &job, error, sizeof(error)) != 0) {
        fprintf(stderr, "%s: %s\n", text("Teşhis sayfası yazdırılamadı", "The diagnostic page could not be printed"), error);
        return 1;
    }
    printf("%s %s-%d\n", text("Teşhis sayfası yazdırılıyor:", "Printing the diagnostic page:"), info.queue, job);
    return 0;
}

int main(int argc, char **argv) {
    const char *language = getenv("P1600DN_LANG");
    english = language && strncasecmp(language, "en", 2) == 0;
    signal(SIGPIPE, SIG_IGN);

    const char *queue_name = NULL, *device_uri = NULL, *sides = NULL, *media = NULL, *output = NULL;
    bool dry_run = false;
    int index = 1;
    for (; index < argc && argv[index][0] == '-'; index++) {
        const char *option = argv[index];
        if (strcmp(option, "--en") == 0 || strcmp(option, "--english") == 0) english = true;
        else if (strcmp(option, "--tr") == 0 || strcmp(option, "--turkish") == 0) english = false;
        else if (strcmp(option, "--dry-run") == 0) dry_run = true;
        else if (strcmp(option, "--queue") == 0 && index + 1 < argc) queue_name = argv[++index];
        else if (strcmp(option, "--device-uri") == 0 && index + 1 < argc) device_uri = argv[++index];
        else if (strcmp(option, "--sides") == 0 && index + 1 < argc) sides = argv[++index];
        else if (strcmp(option, "--media") == 0 && index + 1 < argc) media = argv[++index];
        else if (strcmp(option, "--output") == 0 && index + 1 < argc) output = argv[++index];
        else if (strcmp(option, "--help") == 0 || strcmp(option, "-h") == 0) {
            usage(stdout);
            return 0;
        } else {
            usage(stderr);
            return 2;
        }
    }
    if (index >= argc) {
        usage(stderr);
        return 2;
    }
    const char *command = argv[index];
    const char *argument = index + 1 < argc ? argv[index + 1] : NULL;
    if (strcmp(command, "list") == 0) return list_queues();
    if (strcmp(command, "page") == 0 && argument && strcmp(argument, "diagnostic") == 0) {
        const char *page_language = index + 2 < argc ? argv[index + 2] : (english ? "en" : "tr");
        return diagnostic_page(queue_name, page_language, sides, media, output);
    }

    P1600Queue queue;
    char error[256] = "";
    if (!device_uri) {
        if (p1600_queue_find(queue_name, &queue, error, sizeof(error)) != 0) {
            fprintf(stderr, "%s\n", error);
            return 1;
        }
        device_uri = queue.device_uri;
    }
    if (strcmp(command, "status") == 0) return show_status(device_uri);
    if (strcmp(command, "web") == 0) return open_web(device_uri, dry_run);

    char pjl[256];
    size_t length = 0;
    int minutes = 0;
    if (strcmp(command, "sleep") == 0 && number(argument, &minutes) == 0) {
        length = p1600_utility_format_sleep(minutes, pjl, sizeof(pjl));
        if (minutes == 0) length = 0;  /* the menu has no 0 */
    } else if (strcmp(command, "auto-off") == 0 && argument) {
        if (strcasecmp(argument, "never") == 0 || strcasecmp(argument, "hiçbir") == 0) minutes = 0;
        else if (number(argument, &minutes) != 0) minutes = -1;
        if (minutes >= 0) length = p1600_utility_format_auto_off(minutes, pjl, sizeof(pjl));
    } else if (strcmp(command, "quiet") == 0 && argument &&
               (strcasecmp(argument, "on") == 0 || strcasecmp(argument, "off") == 0)) {
        length = p1600_utility_format_quiet_mode(strcasecmp(argument, "on") == 0, pjl, sizeof(pjl));
    } else if (strcmp(command, "page") == 0 && argument) {
        static const char *const pages[] = {"demo", "configuration", "supplies"};
        const char *page_language = index + 2 < argc ? argv[index + 2] : (english ? "en" : "tr");
        for (int page = 0; page < 3; page++) {
            if (strcmp(argument, pages[page]) == 0) {
                length = p1600_utility_format_info_page((P1600InfoPage)page, p1600_utility_pjl_language(page_language), pjl,
                                                     sizeof(pjl));
            }
        }
    } else if (strcmp(command, "clean") == 0) {
        length = p1600_utility_format_cleaning(pjl, sizeof(pjl));
    }
    if (length == 0) {
        usage(stderr);
        return 2;
    }
    if (dry_run) {
        show_bytes(pjl, length);
        return 0;
    }
    if (p1600_utility_send(device_uri, pjl, length, error, sizeof(error)) != 0) {
        fprintf(stderr, "%s: %s\n", text("Yazıcıya gönderilemedi", "Could not send to the printer"), error);
        return 1;
    }
    puts(text("Yazıcıya gönderildi.", "Sent to the printer."));
    return 0;
}
