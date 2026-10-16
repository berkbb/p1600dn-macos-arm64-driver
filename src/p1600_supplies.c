#include "p1600_supplies.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <cups/cups.h>
#include <cups/http.h>

static void copy_string(char *destination, size_t size, const char *source) {
    snprintf(destination, size, "%s", source ? source : "");
}

int p1600_usb_target_from_uri(const char *uri, P1600UsbTarget *target) {
    if (!uri || !target) return -1;
    memset(target, 0, sizeof(*target));

    char scheme[32], userpass[256], host[256], resource[1024];
    int port = 0;
    if (httpSeparateURI(HTTP_URI_CODING_ALL, uri, scheme, sizeof(scheme), userpass, sizeof(userpass),
                        host, sizeof(host), &port, resource, sizeof(resource)) < HTTP_URI_STATUS_OK) {
        return -1;
    }
    if (strcasecmp(scheme, "usb") != 0) return 1;

    char *query = strchr(resource, '?');
    if (query) *query++ = '\0';
    copy_string(target->make, sizeof(target->make), host);
    copy_string(target->product, sizeof(target->product), resource[0] == '/' ? resource + 1 : resource);
    /* Only the serial value is taken; CUPS URIs may carry more
     * parameters ("serial=...&interface=1"). */
    for (char *parameter = query; parameter && *parameter;) {
        char *next = strchr(parameter, '&');
        if (next) *next++ = '\0';
        if (strncmp(parameter, "serial=", 7) == 0) {
            copy_string(target->serial, sizeof(target->serial), parameter + 7);
        }
        parameter = next;
    }
    return target->product[0] || target->serial[0] ? 0 : -1;
}

static bool product_matches(const P1600UsbTarget *target, const char *usb_product) {
    if (!usb_product || !target->product[0]) return false;
    if (strcasecmp(usb_product, target->product) == 0) return true;
    size_t make_length = strlen(target->make);
    return make_length > 0 && strncasecmp(usb_product, target->make, make_length) == 0 &&
           usb_product[make_length] == ' ' && strcasecmp(usb_product + make_length + 1, target->product) == 0;
}

bool p1600_usb_device_matches(const P1600UsbTarget *target, const char *usb_product,
                           const char *usb_serial, uint32_t vendor_id) {
    if (!target) return false;
    if (target->serial[0]) {
        if (!usb_serial || strcmp(usb_serial, target->serial) != 0) return false;
        return product_matches(target, usb_product) || vendor_id == P1600_USB_VENDOR_ID;
    }
    return product_matches(target, usb_product);
}

/* Buffered byte stream over an P1600ByteReader. */
typedef struct {
    P1600ByteReader read;
    void *context;
    unsigned char buffer[4096];
    size_t position;
    size_t length;
    bool ended;
} HttpStream;

static bool stream_fill(HttpStream *stream) {
    if (stream->position < stream->length) return true;
    if (stream->ended) return false;
    ssize_t received = stream->read(stream->context, stream->buffer, sizeof(stream->buffer));
    if (received <= 0) {
        stream->ended = true;
        return false;
    }
    stream->position = 0;
    stream->length = (size_t)received;
    return true;
}

static size_t stream_read(HttpStream *stream, unsigned char *out, size_t length) {
    size_t done = 0;
    while (done < length && stream_fill(stream)) {
        size_t amount = stream->length - stream->position;
        if (amount > length - done) amount = length - done;
        memcpy(out + done, stream->buffer + stream->position, amount);
        stream->position += amount;
        done += amount;
    }
    return done;
}

/* One header line without CR LF; -1 at the end of the stream or when the
 * line does not fit. */
static int stream_line(HttpStream *stream, char *line, size_t size) {
    size_t length = 0;
    for (;;) {
        if (!stream_fill(stream)) return -1;
        unsigned char c = stream->buffer[stream->position++];
        if (c == '\n') break;
        if (length + 1U >= size) return -1;
        line[length++] = (char)c;
    }
    if (length > 0 && line[length - 1U] == '\r') length--;
    line[length] = '\0';
    return (int)length;
}

/* Appends a header line and CR LF to `head` (NUL-terminated); false when
 * it does not fit. */
static bool keep_line(char *head, size_t head_size, const char *line) {
    if (!head) return true;
    size_t used = strlen(head), length = strlen(line);
    if (used + length + 3U > head_size) return false;
    memcpy(head + used, line, length);
    memcpy(head + used + length, "\r\n", 3);
    return true;
}

int p1600_http_read_response(P1600ByteReader read, void *context, unsigned char *body,
                          size_t capacity, size_t *body_length) {
    return p1600_http_read_message(read, context, NULL, 0, body, capacity, body_length);
}

int p1600_http_read_message(P1600ByteReader read, void *context, char *head, size_t head_size, unsigned char *body,
                         size_t capacity, size_t *body_length) {
    if (!read || !body_length || (!body && capacity != 0) || (head && head_size == 0)) return -1;
    *body_length = 0;
    if (head) head[0] = '\0';

    HttpStream *stream = calloc(1, sizeof(*stream));
    if (!stream) return -1;
    stream->read = read;
    stream->context = context;

    char line[1024];
    int major = 0, minor = 0, status = -1;
    if (stream_line(stream, line, sizeof(line)) < 0 ||
        sscanf(line, "HTTP/%d.%d %d", &major, &minor, &status) != 3 || status < 100 || status > 999 ||
        !keep_line(head, head_size, line)) {
        free(stream);
        return -1;
    }

    bool chunked = false;
    long long content_length = -1;
    int headers = 0;
    for (;;) {
        int length = stream_line(stream, line, sizeof(line));
        if (length < 0 || ++headers > 100) {
            free(stream);
            return -1;
        }
        if (length == 0) break;
        if (!keep_line(head, head_size, line)) {
            free(stream);
            return -1;
        }
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';
        const char *value = colon + 1;
        value += strspn(value, " \t");
        if (strcasecmp(line, "Content-Length") == 0) {
            char *end = NULL;
            errno = 0;
            long long parsed = strtoll(value, &end, 10);
            if (end == value || errno != 0 || parsed < 0) {
                free(stream);
                return -1;
            }
            content_length = parsed;
        } else if (strcasecmp(line, "Transfer-Encoding") == 0 && strcasestr(value, "chunked")) {
            chunked = true;
        }
    }

    size_t total = 0;
    int result = status;
    if (chunked) {
        for (;;) {
            char *end = NULL;
            if (stream_line(stream, line, sizeof(line)) < 0) {
                result = -1;
                break;
            }
            errno = 0;
            unsigned long long size = strtoull(line, &end, 16);
            if (end == line || errno != 0) {
                result = -1;
                break;
            }
            if (size == 0) {
                while (stream_line(stream, line, sizeof(line)) > 0) {
                }
                break;
            }
            if (size > capacity - total || stream_read(stream, body + total, (size_t)size) != size ||
                stream_line(stream, line, sizeof(line)) != 0) {
                result = -1;
                break;
            }
            total += (size_t)size;
        }
    } else if (content_length >= 0) {
        if ((unsigned long long)content_length > capacity ||
            stream_read(stream, body, (size_t)content_length) != (size_t)content_length) {
            result = -1;
        } else {
            total = (size_t)content_length;
        }
    } else {
        while (stream_fill(stream)) {
            size_t amount = stream->length - stream->position;
            if (amount > capacity - total) {
                result = -1;
                break;
            }
            memcpy(body + total, stream->buffer + stream->position, amount);
            stream->position += amount;
            total += amount;
        }
    }
    free(stream);
    if (result >= 0) *body_length = total;
    return result;
}

static const char *skip_space(const char *p, const char *end) {
    while (p < end && isspace((unsigned char)*p)) p++;
    return p;
}

/* Decimal number at `p` (no sign); -1 when there is none or it is too big. */
static long parse_number(const char **p, const char *end) {
    long value = 0;
    const char *start = *p;
    while (*p < end && isdigit((unsigned char)**p)) {
        if (value > 100000000L) return -1;
        value = value * 10 + (**p - '0');
        (*p)++;
    }
    return *p == start ? -1 : value;
}

int p1600_ledm_level(const char *xml, size_t length) {
    static const char tag[] = "<dd:ConsumablePercentageLevelRemaining>";
    if (!xml) return -1;
    const char *end = xml + length;
    const char *p = memmem(xml, length, tag, sizeof(tag) - 1U);
    if (!p) return -1;
    p = skip_space(p + sizeof(tag) - 1U, end);
    long level = parse_number(&p, end);
    p = skip_space(p, end);
    if (level < 0 || level > 100 || p >= end || *p != '<') return -1;
    return (int)level;
}

/* <integer>n</integer> after a plist key; false when malformed. */
static bool plist_integer(const char *p, const char *end, long *value) {
    static const char open[] = "<integer>";
    p = skip_space(p, end);
    if ((size_t)(end - p) < sizeof(open) - 1U || memcmp(p, open, sizeof(open) - 1U) != 0) return false;
    p = skip_space(p + sizeof(open) - 1U, end);
    const char *digits = p;
    bool negative = p < end && *p == '-';
    if (negative) p++;
    long parsed = 0;
    while (p < end && isdigit((unsigned char)*p)) {
        if (parsed > 100000000L) return false;
        parsed = parsed * 10 + (*p - '0');
        p++;
    }
    if (p == digits + (negative ? 1 : 0)) return false;
    p = skip_space(p, end);
    if (p >= end || *p != '<') return false;
    *value = negative ? -parsed : parsed;
    return true;
}

int p1600_snmpink_level(const char *plist, size_t length) {
    static const char level_key[] = "<key>prtMarkerSuppliesLevel</key>";
    static const char capacity_key[] = "<key>prtMarkerSuppliesMaxCapacity</key>";
    if (!plist) return -1;
    const char *end = plist + length;
    const char *level_at = memmem(plist, length, level_key, sizeof(level_key) - 1U);
    long level = 0;
    if (!level_at || !plist_integer(level_at + sizeof(level_key) - 1U, end, &level) || level < 0) return -1;

    /* The supply's own dictionary bounds the capacity lookup. */
    const char *dict_start = plist;
    for (const char *p = level_at; p >= plist + 6; p--) {
        if (memcmp(p - 6, "<dict>", 6) == 0) {
            dict_start = p - 6;
            break;
        }
    }
    const char *dict_end = memmem(level_at, (size_t)(end - level_at), "</dict>", 7);
    if (!dict_end) dict_end = end;
    const char *capacity_at = memmem(dict_start, (size_t)(dict_end - dict_start), capacity_key, sizeof(capacity_key) - 1U);
    long capacity = 0;
    if (capacity_at && plist_integer(capacity_at + sizeof(capacity_key) - 1U, dict_end, &capacity) && capacity > 0) {
        long percent = level * 100L / capacity;
        return (int)(percent > 100L ? 100L : percent);
    }
    return level <= 100L ? (int)level : -1;
}

size_t p1600_supply_attr_line(int level, char *out, size_t size) {
    if (!out || size == 0) return 0;
    out[0] = '\0';
    if (level < 0 || level > 100) return 0;
    int length = snprintf(out, size,
                          "ATTR: marker-colors=#000000 marker-levels=%d marker-names=Black marker-types=toner\n",
                          level);
    if (length < 0 || (size_t)length >= size) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)length;
}
