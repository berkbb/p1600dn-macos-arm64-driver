#include "p1600_diagnostic.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <cups/cups.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#include "p1600_queues.h"

static int fail(char *error, size_t size, const char *format, ...) {
    if (error && size > 0) {
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(error, size, format, arguments);
        va_end(arguments);
    }
    return -1;
}

static void copy(char *destination, size_t size, const char *source) {
    snprintf(destination, size, "%s", source ? source : "");
}

/* ---------------------------------------------------------------- texts */

typedef enum {
    T_TITLE, T_PAGE_FRONT, T_PAGE_BACK, T_SECTION_PRINTER, T_PRINTER, T_QUEUE, T_CONNECTION,
    T_DRIVER, T_PAPER, T_RESOLUTION, T_SIDES, T_SYSTEM, T_DATE, T_GUARD, T_GUARD_ON,
    T_SIDES_ONE, T_SIDES_LONG, T_SIDES_SHORT, T_SECTION_GRAY, T_SECTION_LINES, T_DOT, T_DOTS,
    T_SECTION_ORIENTATION, T_FRONT, T_BACK, T_ORIENTATION_TEXT, T_SECTION_CHECK, T_CHECK_1,
    T_CHECK_2, T_CHECK_3, T_CHECK_4, T_CHECK_5, T_SECTION_GUARD, T_GUARD_TEXT, T_KEEP,
    T_BOTTOM_TITLE, T_BOTTOM_TEXT, T_CORNERS, T_USB, T_NETWORK, T_UNKNOWN, T_COUNT
} TextKey;

static const char *const english[T_COUNT] = {
    [T_TITLE] = "Diagnostic Page",
    [T_PAGE_FRONT] = "Page 1 of 2 — Front",
    [T_PAGE_BACK] = "Page 2 of 2 — Back",
    [T_SECTION_PRINTER] = "Printer and Driver",
    [T_PRINTER] = "Printer",
    [T_QUEUE] = "Queue",
    [T_CONNECTION] = "Connection",
    [T_DRIVER] = "Driver",
    [T_PAPER] = "Paper",
    [T_RESOLUTION] = "Resolution",
    [T_SIDES] = "2-Sided Printing",
    [T_SYSTEM] = "System",
    [T_DATE] = "Printed",
    [T_GUARD] = "Page check",
    [T_GUARD_ON] = "On: every page is verified before it is sent",
    [T_SIDES_ONE] = "Off (1-Sided)",
    [T_SIDES_LONG] = "Long-Edge (Portrait)",
    [T_SIDES_SHORT] = "Short-Edge (Landscape)",
    [T_SECTION_GRAY] = "Gray Levels",
    [T_SECTION_LINES] = "Fine Lines and Text",
    [T_DOT] = "dot",
    [T_DOTS] = "dots",
    [T_SECTION_ORIENTATION] = "Orientation",
    [T_FRONT] = "FRONT (1)",
    [T_BACK] = "BACK (2)",
    [T_ORIENTATION_TEXT] =
        "Long-Edge: turn the sheet over like a book page; both arrows point to the same edge. "
        "Short-Edge: turn it over like a calendar; both arrows point to the same edge. "
        "1-Sided: page 2 is printed on a sheet of its own.",
    [T_SECTION_CHECK] = "Checklist",
    [T_CHECK_1] = "No lines or marks below the black bar at the bottom of either side",
    [T_CHECK_2] = "Every gray level can be told apart from its neighbours",
    [T_CHECK_3] = "The fine lines are unbroken and the small text is readable",
    [T_CHECK_4] = "All four corner marks are printed",
    [T_CHECK_5] = "The back side faces the way described above",
    [T_SECTION_GUARD] = "About the Page Check",
    [T_GUARD_TEXT] =
        "This driver checks every page before it sends it to the printer: the white space below "
        "the page content and at its right edge is forced white, the image size matches what the "
        "printer expects, and the compressed image is decoded and compared with the page, pixel "
        "by pixel. A page that fails the check is never printed.",
    [T_KEEP] = "If something is wrong, keep this sheet: it shows the problem.",
    [T_BOTTOM_TITLE] = "BOTTOM EDGE TEST",
    [T_BOTTOM_TEXT] = "Nothing may be printed below the black bar, down to the edge of the paper.",
    [T_CORNERS] = "The four corner marks show the printable area, 12 pt from each edge.",
    [T_USB] = "USB",
    [T_NETWORK] = "Network",
    [T_UNKNOWN] = "Unknown",
};

static const char *const turkish[T_COUNT] = {
    [T_TITLE] = "Teşhis Sayfası",
    [T_PAGE_FRONT] = "Sayfa 1 / 2 — Ön yüz",
    [T_PAGE_BACK] = "Sayfa 2 / 2 — Arka yüz",
    [T_SECTION_PRINTER] = "Yazıcı ve Sürücü",
    [T_PRINTER] = "Yazıcı",
    [T_QUEUE] = "Kuyruk",
    [T_CONNECTION] = "Bağlantı",
    [T_DRIVER] = "Sürücü",
    [T_PAPER] = "Kağıt",
    [T_RESOLUTION] = "Çözünürlük",
    [T_SIDES] = "Çift Taraflı Yazdırma",
    [T_SYSTEM] = "Sistem",
    [T_DATE] = "Yazdırma tarihi",
    [T_GUARD] = "Sayfa denetimi",
    [T_GUARD_ON] = "Açık: her sayfa gönderilmeden önce doğrulanır",
    [T_SIDES_ONE] = "Kapalı (tek taraflı)",
    [T_SIDES_LONG] = "Uzun kenar (dikey)",
    [T_SIDES_SHORT] = "Kısa kenar (yatay)",
    [T_SECTION_GRAY] = "Gri Tonlar",
    [T_SECTION_LINES] = "İnce Çizgiler ve Metin",
    [T_DOT] = "nokta",
    [T_DOTS] = "nokta",
    [T_SECTION_ORIENTATION] = "Yön",
    [T_FRONT] = "ÖN YÜZ (1)",
    [T_BACK] = "ARKA YÜZ (2)",
    [T_ORIENTATION_TEXT] =
        "Uzun kenar: kağıdı kitap sayfası gibi çevirin; iki ok da aynı kenarı göstermelidir. "
        "Kısa kenar: kağıdı takvim gibi yukarı çevirin; iki ok da aynı kenarı göstermelidir. "
        "Tek taraflı: 2. sayfa ayrı bir kağıda basılır.",
    [T_SECTION_CHECK] = "Kontrol Listesi",
    [T_CHECK_1] = "İki yüzde de alttaki siyah bandın altında çizgi veya iz yok",
    [T_CHECK_2] = "Her gri ton yanındakinden ayırt edilebiliyor",
    [T_CHECK_3] = "İnce çizgiler kesintisiz, küçük yazılar okunuyor",
    [T_CHECK_4] = "Dört köşe işaretinin hepsi basılı",
    [T_CHECK_5] = "Arka yüz yukarıda anlatılan yönde",
    [T_SECTION_GUARD] = "Sayfa Denetimi Hakkında",
    [T_GUARD_TEXT] =
        "Bu sürücü her sayfayı yazıcıya göndermeden önce denetler: sayfa içeriğinin altındaki ve sağ "
        "kenarındaki boşluk beyaz yapılır, görüntü boyutları yazıcının beklediğiyle aynı olmalıdır ve "
        "sıkıştırılmış görüntü açılıp sayfayla piksel piksel karşılaştırılır. Denetimden geçmeyen sayfa "
        "basılmaz.",
    [T_KEEP] = "Bir sorun varsa bu kağıdı saklayın: sorunu gösterir.",
    [T_BOTTOM_TITLE] = "ALT KENAR TESTİ",
    [T_BOTTOM_TEXT] = "Siyah bandın altında, kağıdın kenarına kadar hiçbir şey basılmamalıdır.",
    [T_CORNERS] = "Dört köşe işareti, her kenardan 12 pt içeride yazdırılabilir alanı gösterir.",
    [T_USB] = "USB",
    [T_NETWORK] = "Ağ",
    [T_UNKNOWN] = "Bilinmiyor",
};

static const char *text(const P1600DiagnosticInfo *info, TextKey key) {
    return strcmp(info->language, "tr") == 0 ? turkish[key] : english[key];
}

const char *p1600_diagnostic_language(const char *localization) {
    return localization && strncasecmp(localization, "tr", 2) == 0 &&
                   (localization[2] == '\0' || localization[2] == '_' || localization[2] == '-' || localization[2] == '.')
               ? "tr"
               : "en";
}

/* ---------------------------------------------------------------- paper */

typedef struct {
    const char *name;     /* PPD PageSize */
    const char *pwg;      /* PWG media name */
    const char *label;
    double width;         /* points (the PPD's PaperDimension) */
    double height;
} Paper;

static const Paper papers[] = {
    {"Letter", "na_letter_8.5x11in", "US Letter (8.5 × 11 in)", 612, 792},
    {"Legal", "na_legal_8.5x14in", "US Legal (8.5 × 14 in)", 612, 1008},
    {"A4", "iso_a4_210x297mm", "A4 (210 × 297 mm)", 595, 842},
    {"Executive", "na_executive_7.25x10.5in", "Executive (7.25 × 10.5 in)", 522, 756},
    {"FanFoldGermanLegal", "na_foolscap_8.5x13in", "8.5 × 13 in", 612, 936},
    {"A5", "iso_a5_148x210mm", "A5 (148 × 210 mm)", 420, 595},
    {"A6", "iso_a6_105x148mm", "A6 (105 × 148 mm)", 297, 420},
    {"B5", "jis_b5_182x257mm", "B5 JIS (182 × 257 mm)", 515.905517578125, 728.503967285156},
    {"195x270mm", "om_16k_195x270mm", "16K (195 × 270 mm)", 552.755920410156, 765.354370117188},
    {"184x260mm", "om_16k_184x260mm", "16K (184 × 260 mm)", 521.574829101562, 737.007873535156},
    {"197x273mm", "roc_16k_7.75x10.75in", "16K (197 × 273 mm)", 558.425231933594, 773.858276367188},
    {"Postcard", "jpn_hagaki_100x148mm", "Postcard (100 × 148 mm)", 284, 419},
    {"DoublePostcard", "jpn_oufuku_148x200mm", "Double Postcard (200 × 148 mm)", 567, 420},
    {"Env10", "na_number-10_4.125x9.5in", "Envelope #10", 297, 684},
    {"EnvMonarch", "na_monarch_3.875x7.5in", "Envelope Monarch", 279, 540},
    {"EnvISOB5", "iso_b5_176x250mm", "Envelope B5", 499, 709},
    {"EnvC5", "iso_c5_162x229mm", "Envelope C5", 459, 649},
    {"EnvDL", "iso_dl_110x220mm", "Envelope DL", 312, 624},
};

static void use_paper(P1600DiagnosticInfo *info, const Paper *paper) {
    copy(info->paper, sizeof(info->paper), paper->name);
    copy(info->paper_label, sizeof(info->paper_label), paper->label);
    info->width = paper->width;
    info->height = paper->height;
}

int p1600_diagnostic_set_paper_size(P1600DiagnosticInfo *info, double width, double height) {
    for (size_t i = 0; info && i < sizeof(papers) / sizeof(papers[0]); i++) {
        if (width > papers[i].width - 1.0 && width < papers[i].width + 1.0 && height > papers[i].height - 1.0 &&
            height < papers[i].height + 1.0) {
            use_paper(info, &papers[i]);
            return 0;
        }
    }
    return -1;
}

int p1600_diagnostic_set_paper(P1600DiagnosticInfo *info, const char *name) {
    if (!info || !name || !*name) return -1;
    for (size_t i = 0; i < sizeof(papers) / sizeof(papers[0]); i++) {
        if (strcasecmp(name, papers[i].name) == 0 || strcasecmp(name, papers[i].pwg) == 0) {
            use_paper(info, &papers[i]);
            return 0;
        }
    }
    return -1;
}

const char *p1600_sides_keyword(P1600Sides sides) {
    return sides == P1600_SIDES_LONG_EDGE ? "two-sided-long-edge"
           : sides == P1600_SIDES_SHORT_EDGE ? "two-sided-short-edge"
                                          : "one-sided";
}

int p1600_sides_parse(const char *value, P1600Sides *sides) {
    if (!value || !sides) return -1;
    if (strcasecmp(value, "one") == 0 || strcasecmp(value, "one-sided") == 0) *sides = P1600_SIDES_ONE;
    else if (strcasecmp(value, "long") == 0 || strcasecmp(value, "two-sided-long-edge") == 0) *sides = P1600_SIDES_LONG_EDGE;
    else if (strcasecmp(value, "short") == 0 || strcasecmp(value, "two-sided-short-edge") == 0) *sides = P1600_SIDES_SHORT_EDGE;
    else return -1;
    return 0;
}

/* ------------------------------------------------------------ queue info */

void p1600_diagnostic_defaults(P1600DiagnosticInfo *info, const char *localization) {
    if (!info) return;
    memset(info, 0, sizeof(*info));
    const char *language = p1600_diagnostic_language(localization);
    copy(info->language, sizeof(info->language), language);
    copy(info->make_model, sizeof(info->make_model), "HP LaserJet Professional P1600dn Series");
    use_paper(info, &papers[2]);
    copy(info->resolution, sizeof(info->resolution), "FastRes 1200");
    info->sides = P1600_SIDES_LONG_EDGE;

    char version[32] = "";
    size_t size = sizeof(version);
    if (sysctlbyname("kern.osproductversion", version, &size, NULL, 0) == 0 && version[0]) {
        snprintf(info->system, sizeof(info->system), "macOS %s", version);
    } else {
        copy(info->system, sizeof(info->system), "macOS");
    }
    time_t now = time(NULL);
    struct tm local;
    if (localtime_r(&now, &local)) {
        strftime(info->date, sizeof(info->date), strcmp(language, "tr") == 0 ? "%d.%m.%Y %H:%M" : "%Y-%m-%d %H:%M",
                 &local);
    }
}

/* "USB", or "Network: <host or Bonjour name>"; serial numbers stay off the page. */
static void connection_label(P1600DiagnosticInfo *info, const char *device_uri) {
    char scheme[32] = "", user[64] = "", host[256] = "", resource[256] = "";
    int port = 0;
    if (!device_uri || !*device_uri ||
        httpSeparateURI(HTTP_URI_CODING_ALL, device_uri, scheme, sizeof(scheme), user, sizeof(user), host,
                        sizeof(host), &port, resource, sizeof(resource)) < HTTP_URI_STATUS_OK) {
        copy(info->connection, sizeof(info->connection), text(info, T_UNKNOWN));
        return;
    }
    if (strcmp(scheme, "usb") == 0) {
        copy(info->connection, sizeof(info->connection), text(info, T_USB));
        return;
    }
    /* A Bonjour host is "<service name>._pdl-datastream._tcp.local." */
    char *service = strstr(host, "._");
    if (service) *service = '\0';
    snprintf(info->connection, sizeof(info->connection), "%s: %s", text(info, T_NETWORK), host);
}

int p1600_diagnostic_info_for_queue(const char *queue, P1600DiagnosticInfo *info, char *error, size_t error_size) {
    if (!queue || !*queue || !info) return fail(error, error_size, "No printer queue");
    P1600Queue found;
    if (p1600_queue_find(queue, &found, error, error_size) != 0) return -1;
    copy(info->queue, sizeof(info->queue), found.name);
    if (found.make_model[0]) copy(info->make_model, sizeof(info->make_model), found.make_model);
    connection_label(info, found.device_uri);

    cups_dest_t *dest = cupsGetNamedDest(CUPS_HTTP_DEFAULT, found.name, NULL);
    if (!dest) return 0;
    const char *resolution = cupsGetOption("Resolution", dest->num_options, dest->options);
    if (resolution && strcmp(resolution, "600dpi") == 0) copy(info->resolution, sizeof(info->resolution), "600 dpi");
    cups_dinfo_t *dinfo = cupsCopyDestInfo(CUPS_HTTP_DEFAULT, dest);
    cups_size_t size;
    if (dinfo && cupsGetDestMediaDefault(CUPS_HTTP_DEFAULT, dest, dinfo, 0, &size)) {
        if (p1600_diagnostic_set_paper(info, size.media) != 0) {
            p1600_diagnostic_set_paper_size(info, size.width * 72.0 / 2540.0, size.length * 72.0 / 2540.0);
        }
    }
    cupsFreeDestInfo(dinfo);
    cupsFreeDests(1, dest);
    return 0;
}

/* -------------------------------------------------------------- drawing */

/* Design frame: everything but the corner marks and the bottom edge test is
 * laid out for an A4 content area and scaled down for smaller paper. */
#define FRAME_WIDTH 535.0
#define FRAME_HEIGHT 736.0
#define PRINTABLE_EDGE 12.0

/* A new attributed string (the caller releases it). */
static CFAttributedStringRef attributed(const char *utf8, double size, bool bold) CF_RETURNS_RETAINED;
static CFAttributedStringRef attributed(const char *utf8, double size, bool bold) {
    CFStringRef string = CFStringCreateWithCString(NULL, utf8 ? utf8 : "", kCFStringEncodingUTF8);
    if (!string) string = CFStringCreateWithCString(NULL, "", kCFStringEncodingUTF8);
    CTFontRef font = CTFontCreateWithName(bold ? CFSTR("Helvetica-Bold") : CFSTR("Helvetica"), size, NULL);
    const void *keys[] = {kCTFontAttributeName, kCTForegroundColorFromContextAttributeName};
    const void *values[] = {font, kCFBooleanTrue};
    CFDictionaryRef attributes = CFDictionaryCreate(NULL, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
                                                    &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef result = CFAttributedStringCreate(NULL, string, attributes);
    CFRelease(attributes);
    CFRelease(font);
    CFRelease(string);
    return result;
}

static double text_width(const char *utf8, double size, bool bold) {
    CFAttributedStringRef string = attributed(utf8, size, bold);
    CTLineRef line = CTLineCreateWithAttributedString(string);
    double width = CTLineGetTypographicBounds(line, NULL, NULL, NULL);
    CFRelease(line);
    CFRelease(string);
    return width;
}

/* One line with its baseline at (x, y), cut with an ellipsis at max_width
 * (0 = no limit). */
static void draw_text(CGContextRef c, const char *utf8, double x, double y, double size, bool bold, double max_width) {
    CFAttributedStringRef string = attributed(utf8, size, bold);
    CTLineRef line = CTLineCreateWithAttributedString(string);
    if (max_width > 0 && CTLineGetTypographicBounds(line, NULL, NULL, NULL) > max_width) {
        CFAttributedStringRef dots = attributed("…", size, bold);
        CTLineRef token = CTLineCreateWithAttributedString(dots);
        CTLineRef cut = CTLineCreateTruncatedLine(line, max_width, kCTLineTruncationEnd, token);
        if (cut) {
            CFRelease(line);
            line = cut;
        }
        CFRelease(token);
        CFRelease(dots);
    }
    CGContextSetTextMatrix(c, CGAffineTransformIdentity);
    CGContextSetTextPosition(c, x, y);
    CTLineDraw(line, c);
    CFRelease(line);
    CFRelease(string);
}

static void draw_text_right(CGContextRef c, const char *utf8, double right, double y, double size, bool bold) {
    draw_text(c, utf8, right - text_width(utf8, size, bold), y, size, bold, 0);
}

static void draw_text_centered(CGContextRef c, const char *utf8, double center, double y, double size, bool bold) {
    draw_text(c, utf8, center - text_width(utf8, size, bold) / 2.0, y, size, bold, 0);
}

/* Wrapped text in `rect` (top-aligned). */
static void draw_paragraph(CGContextRef c, const char *utf8, CGRect rect, double size, bool bold) {
    CFAttributedStringRef string = attributed(utf8, size, bold);
    CTFramesetterRef setter = CTFramesetterCreateWithAttributedString(string);
    CGPathRef path = CGPathCreateWithRect(rect, NULL);
    CTFrameRef frame = CTFramesetterCreateFrame(setter, CFRangeMake(0, 0), path, NULL);
    CGContextSetTextMatrix(c, CGAffineTransformIdentity);
    CTFrameDraw(frame, c);
    CFRelease(frame);
    CGPathRelease(path);
    CFRelease(setter);
    CFRelease(string);
}

static void line(CGContextRef c, double x1, double y1, double x2, double y2, double width) {
    CGContextSetLineWidth(c, width);
    CGContextMoveToPoint(c, x1, y1);
    CGContextAddLineToPoint(c, x2, y2);
    CGContextStrokePath(c);
}

/* Frame coordinates are measured from the top of the design frame. */
static double Y(double from_top) {
    return FRAME_HEIGHT - from_top;
}

static void heading(CGContextRef c, const char *title, double top) {
    draw_text(c, title, 0, Y(top), 11, true, FRAME_WIDTH);
    line(c, 0, Y(top + 4), FRAME_WIDTH, Y(top + 4), 0.5);
}

static void header(CGContextRef c, const P1600DiagnosticInfo *info, bool front) {
    draw_text(c, "HP LaserJet Professional P1600dn Series", 0, Y(16), 15, true, FRAME_WIDTH - 150);
    draw_text_right(c, text(info, front ? T_PAGE_FRONT : T_PAGE_BACK), FRAME_WIDTH, Y(16), 9, false);
    draw_text(c, text(info, T_TITLE), 0, Y(44), 22, true, FRAME_WIDTH);
    line(c, 0, Y(54), FRAME_WIDTH, Y(54), 1.0);
}

/* An upward arrow in `rect`. */
static void arrow(CGContextRef c, CGRect rect) {
    double x = rect.origin.x, y = rect.origin.y, w = rect.size.width, h = rect.size.height;
    CGContextBeginPath(c);
    CGContextMoveToPoint(c, x + w * 0.5, y + h);
    CGContextAddLineToPoint(c, x + w, y + h * 0.58);
    CGContextAddLineToPoint(c, x + w * 0.68, y + h * 0.58);
    CGContextAddLineToPoint(c, x + w * 0.68, y);
    CGContextAddLineToPoint(c, x + w * 0.32, y);
    CGContextAddLineToPoint(c, x + w * 0.32, y + h * 0.58);
    CGContextAddLineToPoint(c, x, y + h * 0.58);
    CGContextClosePath(c);
    CGContextFillPath(c);
}

static const char *sides_text(const P1600DiagnosticInfo *info) {
    return text(info, info->sides == P1600_SIDES_LONG_EDGE    ? T_SIDES_LONG
                      : info->sides == P1600_SIDES_SHORT_EDGE ? T_SIDES_SHORT
                                                           : T_SIDES_ONE);
}

static void front_content(CGContextRef c, const P1600DiagnosticInfo *info) {
    header(c, info, true);

    heading(c, text(info, T_SECTION_PRINTER), 76);
    char driver[64];
    snprintf(driver, sizeof(driver), "%s (Native ARM64)", P1600_DRIVER_VERSION);
    const struct {
        TextKey key;
        const char *value;
    } rows[] = {
        {T_PRINTER, info->make_model},
        {T_QUEUE, info->queue[0] ? info->queue : "—"},
        {T_CONNECTION, info->connection[0] ? info->connection : "—"},
        {T_DRIVER, driver},
        {T_PAPER, info->paper_label},
        {T_RESOLUTION, info->resolution},
        {T_SIDES, sides_text(info)},
        {T_SYSTEM, info->system},
        {T_DATE, info->date},
        {T_GUARD, text(info, T_GUARD_ON)},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        double baseline = Y(96 + 14.0 * (double)i);
        draw_text(c, text(info, rows[i].key), 0, baseline, 9.5, true, 145);
        draw_text(c, rows[i].value, 150, baseline, 9.5, false, FRAME_WIDTH - 150);
    }

    heading(c, text(info, T_SECTION_GRAY), 252);
    const double cell = FRAME_WIDTH / 11.0;
    for (int step = 0; step <= 10; step++) {
        double x = cell * step;
        CGRect patch = CGRectMake(x + 2, Y(298), cell - 4, 30);
        CGContextSetGrayFillColor(c, 1.0 - step / 10.0, 1.0);
        CGContextFillRect(c, patch);
        CGContextSetGrayFillColor(c, 0.0, 1.0);
        CGContextSetLineWidth(c, 0.5);
        CGContextStrokeRect(c, patch);
        char label[8];
        snprintf(label, sizeof(label), "%d%%", step * 10);
        draw_text_centered(c, label, x + cell / 2.0, Y(310), 8, false);
    }

    heading(c, text(info, T_SECTION_LINES), 336);
    /* 1, 2, 4 and 8 printer dots at 600 dpi. */
    static const int dots[] = {1, 2, 4, 8};
    for (int i = 0; i < 4; i++) {
        double width = dots[i] * 72.0 / 600.0;
        line(c, 0, Y(356 + 12.0 * i), 200, Y(356 + 12.0 * i), width);
        char label[32];
        snprintf(label, sizeof(label), "%d %s", dots[i], text(info, dots[i] == 1 ? T_DOT : T_DOTS));
        draw_text(c, label, 208, Y(359 + 12.0 * i), 8, false, 70);
        line(c, 12 + 50.0 * i, Y(406), 12 + 50.0 * i, Y(456), width);
        draw_text_centered(c, label, 12 + 50.0 * i, Y(468), 8, false);
    }
    /* A star of 1-dot lines shows how fine detail is resolved. */
    for (int i = 0; i < 36; i++) {
        double angle = i * 3.14159265358979323846 / 18.0;
        line(c, 408, Y(418), 408 + 56 * cos(angle), Y(418) + 56 * sin(angle), 72.0 / 600.0);
    }
    static const double sizes[] = {6, 8, 10, 12, 14};
    for (int i = 0; i < 5; i++) {
        char sample[160];
        snprintf(sample, sizeof(sample),
                 "%g pt  Aa Çç Ğğ İı Öö Şş Üü 0123456789",
                 sizes[i]);
        draw_text(c, sample, 0, Y(494 + 15.0 * i + sizes[i] / 2.0), sizes[i], false, FRAME_WIDTH);
    }

    heading(c, text(info, T_SECTION_ORIENTATION), 590);
    arrow(c, CGRectMake(14, Y(726), 60, 120));
    draw_text(c, text(info, T_FRONT), 100, Y(630), 18, true, FRAME_WIDTH - 100);
    draw_paragraph(c, text(info, T_ORIENTATION_TEXT), CGRectMake(100, Y(726), FRAME_WIDTH - 100, 88), 9.5, false);
}

static void back_content(CGContextRef c, const P1600DiagnosticInfo *info) {
    header(c, info, false);

    heading(c, text(info, T_SECTION_ORIENTATION), 76);
    arrow(c, CGRectMake(14, Y(266), 70, 170));
    draw_text(c, text(info, T_BACK), 110, Y(130), 22, true, FRAME_WIDTH - 110);
    draw_paragraph(c, text(info, T_ORIENTATION_TEXT), CGRectMake(110, Y(266), FRAME_WIDTH - 110, 120), 10, false);

    heading(c, text(info, T_SECTION_CHECK), 300);
    static const TextKey checks[] = {T_CHECK_1, T_CHECK_2, T_CHECK_3, T_CHECK_4, T_CHECK_5};
    for (int i = 0; i < 5; i++) {
        double baseline = Y(324 + 22.0 * i);
        CGContextSetLineWidth(c, 0.75);
        CGContextStrokeRect(c, CGRectMake(0, baseline - 1, 10, 10));
        draw_text(c, text(info, checks[i]), 18, baseline, 10, false, FRAME_WIDTH - 18);
    }
    draw_text(c, text(info, T_CORNERS), 0, Y(448), 9, false, FRAME_WIDTH);

    heading(c, text(info, T_SECTION_GUARD), 482);
    draw_paragraph(c, text(info, T_GUARD_TEXT), CGRectMake(0, Y(590), FRAME_WIDTH, 96), 10, false);
    draw_text(c, text(info, T_KEEP), 0, Y(560), 10, true, FRAME_WIDTH);
}

/* Corner marks of the printable area and the bottom edge test, at the
 * paper's real edges (never scaled). */
static void edges(CGContextRef c, const P1600DiagnosticInfo *info) {
    const double w = info->width, h = info->height, e = PRINTABLE_EDGE, arm = 14;
    CGContextSetGrayStrokeColor(c, 0.0, 1.0);
    line(c, e, e, e + arm, e, 0.75);
    line(c, e, e, e, e + arm, 0.75);
    line(c, w - e, e, w - e - arm, e, 0.75);
    line(c, w - e, e, w - e, e + arm, 0.75);
    line(c, e, h - e, e + arm, h - e, 0.75);
    line(c, e, h - e, e, h - e - arm, 0.75);
    line(c, w - e, h - e, w - e - arm, h - e, 0.75);
    line(c, w - e, h - e, w - e, h - e - arm, 0.75);

    /* The black bar is the last thing printed; below it there must be
     * white paper down to the edge. */
    const double left = e + 8, right = w - e - 8;
    CGContextSetGrayFillColor(c, 0.0, 1.0);
    CGContextFillRect(c, CGRectMake(left, 24, right - left, 10));
    double size = 8;
    char rest[256];
    snprintf(rest, sizeof(rest), " — %s", text(info, T_BOTTOM_TEXT));
    const char *title = text(info, T_BOTTOM_TITLE);
    while (size > 4.5 && text_width(title, size, true) + text_width(rest, size, false) > right - left) size -= 0.5;
    double title_width = text_width(title, size, true);
    draw_text(c, title, left, 40, size, true, 0);
    draw_text(c, rest, left + title_width, 40, size, false, right - left - title_width);
}

static void page(CGContextRef c, const P1600DiagnosticInfo *info, bool front) {
    CGPDFContextBeginPage(c, NULL);
    CGContextSetGrayFillColor(c, 0.0, 1.0);
    CGContextSetGrayStrokeColor(c, 0.0, 1.0);
    edges(c, info);

    /* The design frame between 30 pt side and top margins and the bottom
     * edge test, scaled down (never up) to the paper. */
    const double available_width = info->width - 60, available_height = info->height - 30 - 60;
    double scale = available_width / FRAME_WIDTH;
    if (available_height / FRAME_HEIGHT < scale) scale = available_height / FRAME_HEIGHT;
    if (scale > 1.0) scale = 1.0;
    CGContextSaveGState(c);
    CGContextTranslateCTM(c, (info->width - FRAME_WIDTH * scale) / 2.0, info->height - 30 - FRAME_HEIGHT * scale);
    CGContextScaleCTM(c, scale, scale);
    if (front) front_content(c, info);
    else back_content(c, info);
    CGContextRestoreGState(c);
    CGPDFContextEndPage(c);
}

int p1600_diagnostic_write_pdf(const char *path, const P1600DiagnosticInfo *info, char *error, size_t error_size) {
    if (!path || !*path || !info) return fail(error, error_size, "Nothing to write");
    if (info->width < 144 || info->height < 144 || info->width > 1296 || info->height > 1296) {
        return fail(error, error_size, "Unsupported paper size %.0f x %.0f pt", info->width, info->height);
    }
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    if (!url) return fail(error, error_size, "Invalid output path");

    CFStringRef title = CFStringCreateWithCString(NULL, text(info, T_TITLE), kCFStringEncodingUTF8);
    const void *keys[] = {kCGPDFContextTitle, kCGPDFContextCreator};
    const void *values[] = {title, CFSTR("HP LaserJet Professional P1600dn Series driver " P1600_DRIVER_VERSION)};
    CFDictionaryRef metadata = CFDictionaryCreate(NULL, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks);
    CGRect box = CGRectMake(0, 0, info->width, info->height);
    CGContextRef pdf = CGPDFContextCreateWithURL(url, &box, metadata);
    CFRelease(metadata);
    CFRelease(title);
    CFRelease(url);
    if (!pdf) return fail(error, error_size, "Unable to create %s", path);

    page(pdf, info, true);
    page(pdf, info, false);
    CGPDFContextClose(pdf);
    CGContextRelease(pdf);

    FILE *written = fopen(path, "rb");
    long size = -1;
    if (written && fseek(written, 0, SEEK_END) == 0) size = ftell(written);
    if (written) fclose(written);
    return size > 0 ? 0 : fail(error, error_size, "Unable to write %s", path);
}

int p1600_diagnostic_print(const P1600DiagnosticInfo *info, int *job_id, char *error, size_t error_size) {
    if (!info || !info->queue[0]) return fail(error, error_size, "No printer queue");
    const char *directory = getenv("TMPDIR");
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/p1600dn-diagnostic-XXXXXX.pdf",
                 directory && *directory ? directory : "/tmp") >= (int)sizeof(path)) {
        return fail(error, error_size, "Temporary path is too long");
    }
    int fd = mkstemps(path, 4);
    if (fd < 0) return fail(error, error_size, "Unable to create a temporary file");
    close(fd);

    int status = p1600_diagnostic_write_pdf(path, info, error, error_size);
    if (status == 0) {
        cups_option_t *options = NULL;
        int count = cupsAddOption("sides", p1600_sides_keyword(info->sides), 0, &options);
        count = cupsAddOption("PageSize", info->paper, count, &options);
        int job = cupsPrintFile2(CUPS_HTTP_DEFAULT, info->queue, path, text(info, T_TITLE), count, options);
        cupsFreeOptions(count, options);
        if (job <= 0) {
            status = fail(error, error_size, "%s", cupsLastErrorString());
        } else if (job_id) {
            *job_id = job;
        }
    }
    unlink(path);
    return status;
}
