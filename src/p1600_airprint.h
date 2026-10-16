#ifndef P1600_AIRPRINT_H
#define P1600_AIRPRINT_H

/*
 * AirPrint and IPP Everywhere for the P1600dn series CUPS queues: one IPP/2.0
 * printer per queue at /ipp/print/<queue> (IPP over HTTP/1.1 with libcups),
 * advertised over Bonjour. Jobs go to the local CUPS queue, so every page
 * still passes rastertop1600dn and its page guard; this server never talks
 * to the printer. macOS cupsd cannot convert Apple raster for PPD queues and
 * offers no AirPrint for shared queues, so URF and PWG raster become PDF here
 * (src/p1600_raster_doc.c) while PDF, JPEG and PNG go to CUPS unchanged.
 *
 * In test mode (spool_dir) documents and their CUPS options are written to a
 * directory instead of CUPS.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <cups/cups.h>

#define P1600_AIR_DEFAULT_PORT 8632
#define P1600_AIR_MAX_QUEUES 16
#define P1600_AIR_MAX_CLIENTS 32
#define P1600_AIR_IDLE_SECONDS 30
#define P1600_AIR_DOCUMENT_LIMIT ((size_t)256 * 1024 * 1024)
#define P1600_AIR_IPP_LIMIT ((size_t)1024 * 1024)
#define P1600_AIR_JOB_HISTORY 200
#define P1600_AIR_MAX_ACTIVE_JOBS 100
#define P1600_AIR_QUEUE_LIMIT ((size_t)1024 * 1024 * 1024) /* documents waiting for the worker */
#define P1600_AIR_STATE_CACHE_SECONDS 2
#define P1600_AIR_MULTIPLE_OPERATION_TIMEOUT 300
#define P1600_AIR_MARGIN 423 /* 12 pt in hundredths of millimetres */

#define P1600_AIR_URF_SUPPORTED "V1.4,W8,SRGB24,RS300-600,CP1,DM1,IS1,MT1-2-3-4-5-8,OB10,PQ4-5"
#define P1600_AIR_PDL "application/pdf,image/jpeg,image/png,image/urf,image/pwg-raster,application/octet-stream"

/* --- media ---------------------------------------------------------------- */

/* One of the PPD's 18 paper sizes. */
typedef struct {
    const char *ppd;    /* PageSize choice */
    const char *pwg;    /* PWG 5101.1 media keyword */
    int width, length;  /* hundredths of millimetres */
} P1600AirMedia;

extern const P1600AirMedia p1600_air_media[];
extern const size_t p1600_air_media_count;

/* By PWG keyword (also PPD and legacy names such as "A4" or "iso-a4"). */
const P1600AirMedia *p1600_air_media_by_name(const char *name);
const P1600AirMedia *p1600_air_media_by_ppd(const char *page_size);
/* By size within 1 mm, in either orientation. */
const P1600AirMedia *p1600_air_media_by_size(int width, int length);

/* IPP media-type keywords and the PPD MediaType choices they select. */
typedef struct {
    const char *ipp;
    const char *ppd;
} P1600AirMediaType;

extern const P1600AirMediaType p1600_air_media_types[];
extern const size_t p1600_air_media_type_count;

const char *p1600_air_media_type_ppd(const char *keyword);
/* media-source auto/manual: PPD InputSlot Auto/Manual. */
const char *p1600_air_media_source_ppd(const char *keyword);

/* --- documents and jobs ------------------------------------------------------ */

/* The supported MIME type the data starts like, or NULL. */
const char *p1600_air_detect_format(const unsigned char *data, size_t length);

/* CUPS options for the job template attributes of `job` (the accepted job
 * group): copies, sides, PageSize, InputSlot, MediaType, Resolution,
 * page-ranges, orientation-requested and print-scaling. Returns the count. */
int p1600_air_job_options(ipp_t *job, int num_options, cups_option_t **options);

/* --- Bonjour ------------------------------------------------------------------ */

/* urn:uuid:... derived from the host and queue names alone. */
void p1600_air_uuid(const char *host, const char *queue, char *uuid, size_t size);
/* "<info> @ <computer>", cut to the 63 bytes of a DNS-SD instance name. */
void p1600_air_service_name(const char *info, const char *computer, char *name, size_t size);

typedef struct {
    const char *resource;    /* rp, e.g. "ipp/print/Queue" */
    const char *make_model;  /* ty, product */
    const char *location;    /* note */
    const char *admin_url;
    const char *uuid;        /* without "urn:uuid:" */
} P1600AirTxt;

/* The _ipp._tcp TXT record. Returns 0 and its length, or -1. */
int p1600_air_txt_record(const P1600AirTxt *txt, unsigned char *buffer, size_t size, uint16_t *length);

/* --- the local CUPS (real mode) --------------------------------------------- */

/* `server` NULL or "" means the system cupsd (cupsServer(), ippPort()). */
typedef struct {
    const char *server;
    int port;
} P1600AirCups;

/* Submits a document to the CUPS queue: the CUPS job id, or -1 with a reason. */
int p1600_air_cups_submit(const P1600AirCups *cups, const char *queue, const char *title, const char *format, int fd,
                       int num_options, cups_option_t *options, char *error, size_t error_size);
/* The state of a CUPS job: 0, 1 when it no longer exists, -1 on errors. */
int p1600_air_cups_job_state(const P1600AirCups *cups, int job_id, ipp_jstate_t *state, char *message,
                          size_t message_size);
int p1600_air_cups_cancel(const P1600AirCups *cups, const char *queue, int job_id);
/* The queue's state, reasons, message, location, media-default and marker
 * attributes, or NULL. */
ipp_t *p1600_air_cups_printer(const P1600AirCups *cups, const char *queue);

/* --- the server ------------------------------------------------------------------ */

typedef struct {
    char name[128];         /* CUPS queue name */
    char info[256];         /* printer-info */
    char make_model[256];   /* printer-make-and-model */
} P1600AirQueueSpec;

typedef struct {
    int port;               /* 0: any free port */
    bool loopback;          /* 127.0.0.1 and ::1 only */
    bool dnssd;
    bool local_only;        /* Bonjour on kDNSServiceInterfaceIndexLocalOnly */
    int verbose;
    const char *spool_dir;  /* test mode: documents and options go here */
    const char *icon_path;
    bool mirror_cups;       /* state and markers from the CUPS queues */
    P1600AirCups cups;
    P1600AirQueueSpec queues[P1600_AIR_MAX_QUEUES];
    int queue_count;
} P1600AirConfig;

typedef struct P1600AirServer P1600AirServer;

/* Binds the listening sockets and starts the job worker; NULL with a reason. */
P1600AirServer *p1600_air_server_create(const P1600AirConfig *config, char *error, size_t error_size);
int p1600_air_server_port(const P1600AirServer *server);
/* Accepts connections and runs Bonjour until `stop_fd` becomes readable. */
int p1600_air_server_run(P1600AirServer *server, int stop_fd);
/* Deregisters, closes every connection, joins the threads and frees. */
void p1600_air_server_free(P1600AirServer *server);

/* Diagnostics on stderr with an "airprint: " prefix. */
void p1600_air_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

#endif /* P1600_AIRPRINT_H */
