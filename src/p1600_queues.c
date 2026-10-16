#include "p1600_queues.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <cups/cups.h>

bool p1600_queue_is_p1600dn(const char *make_model) {
    static const char *const models[] = {"P1600dn", "P1606dn", "P1607dn", "P1608dn", "P1609dn"};
    if (!make_model) return false;
    for (size_t i = 0; i < sizeof(models) / sizeof(models[0]); i++) {
        if (strcasestr(make_model, models[i])) return true;
    }
    return false;
}

int p1600_queue_first_level(const char *marker_levels) {
    if (!marker_levels) return -1;
    char *end = NULL;
    long level = strtol(marker_levels, &end, 10);
    if (end == marker_levels || (*end != '\0' && *end != ',') || level < 0 || level > 100) return -1;
    return (int)level;
}

static void copy_option(char *out, size_t size, const char *name, const cups_dest_t *dest) {
    const char *value = cupsGetOption(name, dest->num_options, dest->options);
    snprintf(out, size, "%s", value ? value : "");
}

static void fill(P1600Queue *queue, const cups_dest_t *dest) {
    memset(queue, 0, sizeof(*queue));
    snprintf(queue->name, sizeof(queue->name), "%s", dest->name);
    copy_option(queue->info, sizeof(queue->info), "printer-info", dest);
    copy_option(queue->make_model, sizeof(queue->make_model), "printer-make-and-model", dest);
    copy_option(queue->device_uri, sizeof(queue->device_uri), "device-uri", dest);
    queue->is_default = dest->is_default != 0;
    const char *state = cupsGetOption("printer-state", dest->num_options, dest->options);
    queue->state = state ? atoi(state) : 0;
    queue->toner_level = p1600_queue_first_level(cupsGetOption("marker-levels", dest->num_options, dest->options));
}

int p1600_queue_list(P1600Queue *queues, int capacity) {
    if (!queues || capacity < 0) return -1;
    cups_dest_t *dests = NULL;
    int count = cupsGetDests(&dests);
    int found = 0;
    /* The default queue first, then the others in CUPS order. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < count; i++) {
            const cups_dest_t *dest = &dests[i];
            if (dest->instance || (pass == 0) != (dest->is_default != 0)) continue;
            const char *make_model = cupsGetOption("printer-make-and-model", dest->num_options, dest->options);
            if (!p1600_queue_is_p1600dn(make_model)) continue;
            if (found < capacity) fill(&queues[found], dest);
            found++;
        }
    }
    cupsFreeDests(count, dests);
    return found;
}

int p1600_queue_find(const char *name, P1600Queue *queue, char *error, size_t error_size) {
    if (!queue) return -1;
    if (!name || !*name) {
        if (p1600_queue_list(queue, 1) >= 1) return 0;
        if (error && error_size > 0) snprintf(error, error_size, "No P1600dn series printer queue found");
        return -1;
    }
    cups_dest_t *dest = cupsGetNamedDest(CUPS_HTTP_DEFAULT, name, NULL);
    if (!dest) {
        if (error && error_size > 0) snprintf(error, error_size, "Printer queue \"%s\" not found", name);
        return -1;
    }
    fill(queue, dest);
    cupsFreeDests(1, dest);
    if (!p1600_queue_is_p1600dn(queue->make_model)) {
        if (error && error_size > 0) {
            snprintf(error, error_size, "Printer queue \"%s\" is not a P1600dn series printer", name);
        }
        return -1;
    }
    return 0;
}
