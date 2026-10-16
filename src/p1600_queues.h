#ifndef P1600_QUEUES_H
#define P1600_QUEUES_H

/*
 * The CUPS queues of this Mac that drive a P1600dn series printer (by their
 * make and model), with the device URI the utility talks to. The queues
 * the user already set up name the printers; there is no USB or Bonjour
 * discovery of its own.
 */

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char name[128];         /* CUPS queue name */
    char info[256];         /* printer-info, the name shown in macOS */
    char make_model[256];   /* printer-make-and-model */
    char device_uri[1024];
    bool is_default;
    int state;              /* printer-state: 3 idle, 4 printing, 5 stopped; 0 unknown */
    int toner_level;        /* toner in percent (first marker-levels value); -1 unknown */
} P1600Queue;

/* The first value of a CUPS marker-levels list ("45" or "45,80") in
 * percent, or -1 when it is missing or not a level (CUPS uses -1 to -3 for
 * unknown levels). */
int p1600_queue_first_level(const char *marker_levels);

/* True for printer-make-and-model strings of the P1600dn series drivers. */
bool p1600_queue_is_p1600dn(const char *make_model);

/* Fills `queues` (up to `capacity`) with the P1600dn series queues; the
 * default queue first. Returns how many there are, or -1. */
int p1600_queue_list(P1600Queue *queues, int capacity);

/* The queue with this name, or the default P1600dn queue for NULL/"".
 * Returns 0, or -1 with a reason. */
int p1600_queue_find(const char *name, P1600Queue *queue, char *error, size_t error_size);

#endif /* P1600_QUEUES_H */
