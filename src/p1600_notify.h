#ifndef P1600_NOTIFY_H
#define P1600_NOTIFY_H

/*
 * Printer status notices for the P1600dn Notifier agent. The driver's filter
 * reports the printer's state with `STATE:` and `INFO:` lines
 * (reference/zjstream/ZJS_STREAM_SPEC.md, section 6), which cupsd
 * keeps as printer-state-reasons and printer-state-message; the command
 * filter's ReportLevels answer becomes marker-levels (section 7).
 *
 * This core reads those attributes from the local cupsd over IPP, follows a
 * queue with an ippget subscription, asks the driver for the toner level,
 * and decides which changes deserve a notice (p1600_notify_compare(), pure and
 * unit-tested). The agent localizes and shows the notices.
 */

#include <stdbool.h>
#include <stddef.h>
#include <cups/cups.h>

#define P1600_NOTIFY_QUEUE_SIZE 128
#define P1600_NOTIFY_MAX_REASONS 16
#define P1600_NOTIFY_REASON_SIZE 64
#define P1600_NOTIFY_MESSAGE_SIZE 512

/* Results of the IPP calls; successful calls return 0 or a count. */
typedef enum {
    P1600_NOTIFY_OK = 0,
    P1600_NOTIFY_FAILED = -1,       /* cupsd answered with another error status */
    P1600_NOTIFY_NOT_FOUND = -2,    /* client-error-not-found: no such queue or subscription */
    P1600_NOTIFY_UNREACHABLE = -3,  /* no IPP answer: cupsd is down or the connection broke */
} P1600NotifyStatus;

/* One observation of a queue. */
typedef struct {
    char queue[P1600_NOTIFY_QUEUE_SIZE];
    char info[256];                    /* printer-info, the name macOS shows */
    int state;                         /* printer-state: 3 idle, 4 processing, 5 stopped; 0 unknown */
    int reason_count;
    /* printer-state-reasons in lower case, without "none" and duplicates,
     * suffixes kept ("media-empty-error"). */
    char reasons[P1600_NOTIFY_MAX_REASONS][P1600_NOTIFY_REASON_SIZE];
    char message[P1600_NOTIFY_MESSAGE_SIZE]; /* printer-state-message, one line */
    int toner;                         /* 0-100 from marker-levels, -1 unknown */
    bool has_data;                     /* false until cupsd answered */
} P1600PrinterSnapshot;

typedef enum {
    P1600_TONER_UNKNOWN = -1,
    P1600_TONER_OK = 0,        /* over 15% */
    P1600_TONER_LOW,           /* 15% or less */
    P1600_TONER_VERY_LOW,      /* 5% or less */
    P1600_TONER_EMPTY,         /* 0%, or a toner-empty / marker-supply-empty reason */
} P1600TonerBucket;

typedef enum {
    P1600_NOTICE_ATTENTION = 0,       /* a reason of interest appeared; `reason` names it */
    P1600_NOTICE_TONER_LOW,
    P1600_NOTICE_TONER_VERY_LOW,
    P1600_NOTICE_TONER_EMPTY,
    P1600_NOTICE_CARTRIDGE_REPLACED,  /* the level rose from 15% or less to over 50% */
} P1600NoticeKind;

typedef struct {
    P1600NoticeKind kind;
    char reason[P1600_NOTIFY_REASON_SIZE];   /* base reason of an attention notice ("media-jam"), else "" */
    char message[P1600_NOTIFY_MESSAGE_SIZE]; /* printer-state-message to show; "" selects the generic text */
    int level;                            /* toner level, -1 unknown */
    /* A plain connecting-to-device (no -error form) is what every backend
     * sets for a moment while it connects: show it only when it is still
     * active P1600_NOTIFY_SETTLE_SECONDS later (p1600_notify_attention_active()). */
    bool tentative;
} P1600Notice;

#define P1600_NOTIFY_SETTLE_SECONDS 15
#define P1600_NOTIFY_MAX_NOTICES 16

/* --- decision logic (pure) ------------------------------------------------ */

/* Copies `keyword` without a -error/-warning/-report suffix. */
void p1600_notify_reason_base(const char *keyword, char *base, size_t size);

/* True for the base reasons that raise an attention notice. */
bool p1600_notify_reason_of_interest(const char *base);

/* True when the snapshot shows `base` in a form that raises a notice:
 * present with any suffix, and for connecting-to-device only in its -error
 * form or together with a printer-state-message. */
bool p1600_notify_attention_active(const P1600PrinterSnapshot *snapshot, const char *base);

P1600TonerBucket p1600_notify_toner_bucket(const P1600PrinterSnapshot *snapshot);

/* Notices for the change from `before` to `after` (at most `max`), attention
 * notices first. A reason of interest that is newly active gives an
 * attention notice; a worse toner bucket gives a toner notice (an unknown
 * earlier level counts as ok, and a toner-empty attention notice replaces
 * the empty one); a rise from 15% or less to over 50% gives "cartridge
 * replaced". Cleared states give nothing. A `before` without data only
 * primes: the attention reasons already active in `after` are reported once,
 * toner is not. Returns the number of notices. */
int p1600_notify_compare(const P1600PrinterSnapshot *before, const P1600PrinterSnapshot *after, P1600Notice *notices, int max);

/* The agent remembers per queue, across restarts, the last known toner
 * bucket. A toner notice is shown only when its bucket is worse than that
 * memory (P1600_TONER_UNKNOWN counts as ok), so a restart does not repeat it;
 * other notices always pass. */
bool p1600_notify_should_post(const P1600Notice *notice, P1600TonerBucket remembered);

/* The memory after observing `after`: its bucket when known, else unchanged. */
P1600TonerBucket p1600_notify_remember(P1600TonerBucket remembered, const P1600PrinterSnapshot *after);

/* At the first observation after a start p1600_notify_compare() only primes,
 * yet the level may have fallen while the agent was not watching (logged
 * out, another user's agent asked for it). A bucket worse than the
 * remembered one (P1600_TONER_UNKNOWN counts as ok) gives its toner notice; an
 * empty one that a toner-empty or marker-supply-empty reason already
 * reports gives none. Returns 1 with `notice` filled, else 0. */
int p1600_notify_prime_toner(P1600TonerBucket remembered, const P1600PrinterSnapshot *after, P1600Notice *notice);

/* The attention reasons active in `snapshot` as a comma-separated list of
 * bases in table order ("media-jam,offline"). The agent remembers it per
 * queue across restarts: at the first observation after a start, a reason
 * from that list is not reported again (a stale offline-report would
 * otherwise come back at every login). Returns the length, 0 when empty or
 * when `size` is too small. */
size_t p1600_notify_active_reasons(const P1600PrinterSnapshot *snapshot, char *out, size_t size);

/* True when `base` is an entry of such a list. */
bool p1600_notify_reason_listed(const char *list, const char *base);

/* Keeps the last known toner level when `after` has none (marker-levels
 * missing for a moment), so a later rise can still count as a new cartridge. */
void p1600_notify_carry_toner(P1600PrinterSnapshot *after, const P1600PrinterSnapshot *before);

/* Stable name of a notice: "attention.<reason>", "toner.low",
 * "toner.very-low", "toner.empty" or "toner.replaced". */
size_t p1600_notice_name(const P1600Notice *notice, char *out, size_t size);

/* --- IPP with the local cupsd -------------------------------------------- */

/* Points libcups at `server` ("host:port" or a socket path) in this thread
 * and, through CUPS_SERVER, in every thread created afterwards. */
void p1600_notify_set_server(const char *server);

/* A blocking connection to cupsServer()/ippPort(), or NULL. */
http_t *p1600_notify_connect(int timeout_ms);

/* Get-Printer-Attributes on ipp://localhost/printers/<queue>: printer-state,
 * -state-reasons, -state-message, -info and marker-levels/-types/-names.
 * Fills `snapshot` (has_data only on success) and returns an P1600NotifyStatus. */
int p1600_notify_snapshot(http_t *http, const char *queue, P1600PrinterSnapshot *snapshot);

/* Create-Printer-Subscriptions for `queue` with notify-pull-method ippget
 * and the events printer-state-changed, printer-stopped and job-completed.
 * Returns the lease cupsd granted in seconds (0: no expiry) or an
 * P1600NotifyStatus below 0. Renew before the lease ends. */
int p1600_notify_subscribe(http_t *http, const char *queue, int lease_seconds, int *subscription_id);

/* Renew-Subscription; returns the granted lease or an P1600NotifyStatus. */
int p1600_notify_renew(http_t *http, int subscription_id, int lease_seconds);

/* Cancel-Subscription; returns an P1600NotifyStatus. */
int p1600_notify_cancel(http_t *http, int subscription_id);

typedef enum {
    P1600_EVENT_OTHER = 0,
    P1600_EVENT_PRINTER_STATE_CHANGED,
    P1600_EVENT_PRINTER_STOPPED,
    P1600_EVENT_JOB_COMPLETED,
} P1600NotifyEventKind;

typedef struct {
    P1600NotifyEventKind kind;
    char queue[P1600_NOTIFY_QUEUE_SIZE]; /* printer-name or the last part of the printer URI; "" when absent */
    int sequence;      /* notify-sequence-number */
    int job_id;        /* notify-job-id of job events, else 0 */
    bool levels_job;   /* job-name is ReportLevels: a toner query, the agent's or another user's */
} P1600NotifyEvent;

/* A job-completed event that should bring a toner query (a ReportLevels
 * command job after printing): not one of the toner queries themselves,
 * this agent's (`own_job`) or another user's, whose end would bring the
 * next query, and so on every interval. */
bool p1600_notify_event_wants_levels(const P1600NotifyEvent *event, int own_job);

/* When to ask for the toner level: `wanted` (a job's end plus a delay), but
 * not before `asked` + `interval` when the agent asked before (`asked` < 0:
 * never). A query that comes too soon is put off, not dropped, or the level
 * after the last job of a burst would wait for yet another job. */
double p1600_notify_levels_time(double wanted, double asked, double interval);

/* Waits up to `wait_seconds` for events of the subscription with
 * Get-Notifications (notify-wait true), asking for the numbers from
 * `*sequence` on (start at 1) and advancing it past what was received;
 * repeated events are dropped. A server that holds the request until an
 * event arrives is asked again until the time is up. cupsd answers at once
 * and names a notify-get-interval; the call then sleeps for the shorter of
 * that interval and the time left. Returns the number of events stored (at
 * most `max`) or an P1600NotifyStatus (P1600_NOTIFY_NOT_FOUND: subscribe again). */
int p1600_notify_wait(http_t *http, int subscription_id, int *sequence, int wait_seconds, P1600NotifyEvent *events,
                   int max);

/* The CUPS command file of a toner level query (ReportLevels). */
#define P1600_NOTIFY_REPORT_LEVELS "#CUPS-COMMAND\nReportLevels\n"

/* Submits P1600_NOTIFY_REPORT_LEVELS to `queue` as application/vnd.cups-command
 * (Create-Job, then cupsStartDocument/cupsWriteRequestData/
 * cupsFinishDocument) on this thread's default connection, so cupsd runs
 * the driver's command filter and updates marker-levels. Returns the job id
 * (> 0) or an P1600NotifyStatus. */
int p1600_notify_request_levels(const char *queue);

/* How long a ReportLevels job may take. Unfinished by then, the printer is
 * off or unreachable: its backend would wait for the printer, keep the job
 * processing and report connecting-to-device as if the user's printer had
 * a problem. Below the 15 s a notice of that reason waits
 * (P1600_NOTIFY_SETTLE_SECONDS), so the agent's own job never causes one. */
#define P1600_NOTIFY_LEVELS_TIMEOUT_SECONDS 12

/* Cancels the ReportLevels job `job_id` on `queue` (cupsd then clears
 * connecting-to-device). P1600_NOTIFY_OK also when the job has finished. */
int p1600_notify_cancel_levels(const char *queue, int job_id);

#endif /* P1600_NOTIFY_H */
