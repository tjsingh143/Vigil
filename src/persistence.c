#include "vigil/persistence.h"
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <libpq-fe.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>

#define IO_TIMEOUT_MS 2000U
#define RETRY_MS 1000U
#define DRAIN_MS 1000U
struct batch {
    char json[160000];
    size_t used;
    struct vigil_queue_stats captured;
    uint64_t revision;
    bool snapshot, gap;
};

static bool expired(struct vigil_queue *q, uint64_t deadline)
{
    uint64_t now = vigil_now_ms();
    (void)pthread_mutex_lock(&q->mutex);
    bool done = q->stats.stopping && now >= q->stop_deadline_ms;
    (void)pthread_mutex_unlock(&q->mutex);
    return done || now >= deadline;
}

static bool socket_wait(struct vigil_queue *q, PGconn *conn, short events, uint64_t deadline)
{
    while (!expired(q, deadline)) {
        struct pollfd fd = {.fd = PQsocket(conn), .events = events};
        if (fd.fd < 0) return false;
        int result = poll(&fd, 1, 50); /* shutdown/deadline rechecked every <=50 ms */
        if (result < 0 && errno == EINTR) continue;
        if (result < 0 || (fd.revents & POLLNVAL) != 0) return false;
        if (result > 0) return true; /* libpq consumes errors/hangup */
    }
    return false;
}

static void discard_notice(void *context, const char *message)
{
    /* libpq's default notice sink is blocking stderr; health errors use SQLSTATE. */
    (void)context;
    (void)message;
}

static PGconn *connect_db(struct vigil_persistence *p)
{
    const char *keys[] = {"host", "hostaddr", "port", "dbname", "user",
        "connect_timeout", "options", "application_name", "gssencmode", NULL};
    const char *values[] = {p->host, p->network ? p->host : "", p->port,
        p->database, p->user, "2",
        "-c statement_timeout=1000 -c lock_timeout=500 -c search_path=pg_catalog",
        "vigild", "disable", NULL};
    uint64_t deadline = vigil_now_ms() + IO_TIMEOUT_MS;
    PGconn *conn = PQconnectStartParams(keys, values, 0);
    if (conn == NULL) return NULL;
    (void)PQsetNoticeProcessor(conn, discard_notice, NULL);
    PostgresPollingStatusType status = PGRES_POLLING_WRITING;
    while (status != PGRES_POLLING_OK) {
        if (PQstatus(conn) == CONNECTION_BAD || status == PGRES_POLLING_FAILED ||
            !socket_wait(&p->queue, conn,
                status == PGRES_POLLING_READING ? POLLIN : POLLOUT, deadline)) {
            PQfinish(conn);
            return NULL;
        }
        status = PQconnectPoll(conn);
    }
    if (PQsetnonblocking(conn, 1) != 0) { PQfinish(conn); return NULL; }
    return conn;
}

static bool write_db(struct vigil_queue *q, PGconn *conn, const char *json, char error[6])
{
    uint64_t deadline = vigil_now_ms() + IO_TIMEOUT_MS;
    memcpy(error, "08006", 6);
    const char *values[] = {json};
    if (PQsendQueryParams(conn, "SELECT vigil.write_batch($1::jsonb)", 1,
                          NULL, values, NULL, NULL, 0) == 0) return false;
    int flush;
    while ((flush = PQflush(conn)) == 1) {
        if (!socket_wait(q, conn, POLLIN | POLLOUT, deadline) ||
            PQconsumeInput(conn) == 0) return false;
    }
    if (flush < 0) return false;
    bool ok = true;
    unsigned results = 0;
    for (;;) {
        while (PQisBusy(conn) != 0) {
            if (!socket_wait(q, conn, POLLIN, deadline) || PQconsumeInput(conn) == 0)
                return false;
        }
        PGresult *result = PQgetResult(conn);
        if (result == NULL) break;
        ++results;
        if (PQresultStatus(result) != PGRES_TUPLES_OK) {
            ok = false;
            const char *code = PQresultErrorField(result, PG_DIAG_SQLSTATE);
            if (code != NULL && strlen(code) == 5) memcpy(error, code, 6);
        }
        PQclear(result);
        if (expired(q, deadline)) return false;
    }
    return ok && results == 1U && PQstatus(conn) == CONNECTION_OK;
}

static void append(struct batch *b, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
static void append(struct batch *b, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int n = vsnprintf(b->json + b->used, sizeof(b->json) - b->used, format, args);
    va_end(args);
    /* Internal fixed bounds, not remotely supplied JSON/text. */
    if (n < 0 || (size_t)n >= sizeof(b->json) - b->used) abort();
    b->used += (size_t)n;
}

static void append_record(struct batch *b, const struct vigil_record *r, bool comma)
{
    append(b, "%s{\"seq\":%" PRIu64 ",\"mono_ms\":%" PRIu64
              ",\"utc_ms\":%" PRIu64 ",\"pod\":%u,\"channel\":%d,"
              "\"kind\":\"%s\",\"detail\":%s}", comma ? "," : "",
           r->seq, r->mono_ms, r->utc_ms, r->pod, r->channel, r->kind, r->detail);
}

static void make_batch(struct vigil_persistence *p, struct batch *b, uint64_t id,
                        uint64_t saved_revision, bool recovery)
{
    struct vigil_record records[VIGIL_BATCH_SIZE], alarms[VIGIL_ALARM_CAPACITY];
    size_t count = vigil_queue_take(&p->queue, records);
    size_t alarm_count;
    (void)pthread_mutex_lock(&p->queue.mutex);
    b->captured = p->queue.stats;
    b->revision = p->queue.alarm_revision;
    alarm_count = p->queue.alarm_count;
    memcpy(alarms, p->queue.alarms, alarm_count * sizeof(*alarms));
    (void)pthread_mutex_unlock(&p->queue.mutex);
    const struct vigil_queue_stats *s = &b->captured;
    uint64_t reading_drops = s->readings_dropped - s->acknowledged_reading_drops;
    uint64_t event_drops = s->events_dropped - s->acknowledged_event_drops;
    b->gap = reading_drops + event_drops != 0;
    b->snapshot = recovery || b->gap || saved_revision != b->revision;
    b->used = 0;
    uint64_t now = vigil_now_ms();
    append(b, "{\"run_id\":\"%s\",\"batch_id\":%" PRIu64
              ",\"started_ms\":%" PRIu64 ",\"started_utc_ms\":%" PRIu64
              ",\"mono_ms\":%" PRIu64 ",\"utc_ms\":%" PRIu64
              ",\"kind\":\"%s\",\"reading_drops\":%" PRIu64
              ",\"event_drops\":%" PRIu64 ",\"health\":{"
              "\"readings_queued\":%" PRIu64 ",\"events_queued\":%" PRIu64
              ",\"readings_dropped\":%" PRIu64 ",\"events_dropped\":%" PRIu64
              ",\"first_drop_ms\":%" PRIu64 ",\"last_drop_ms\":%" PRIu64
              ",\"outage_started_ms\":%" PRIu64 ",\"outage_ms\":%" PRIu64
              ",\"outage_total_ms\":%" PRIu64 ",\"outage_ended_ms\":%" PRIu64
              ",\"connection_failures\":%" PRIu64 ",\"query_failures\":%" PRIu64
              ",\"reconnects\":%" PRIu64 ",\"revision\":%" PRIu64 "},\"records\":[",
           p->run_id, id, p->started_ms, p->started_utc_ms, now, vigil_utc_ms(),
           b->gap ? "gap" : recovery ? "recovery" : "flush", reading_drops, event_drops,
           s->readings_queued, s->events_queued, s->readings_dropped, s->events_dropped,
           s->first_drop_ms, s->last_drop_ms,
           s->outage_started_ms != 0 ? s->outage_started_ms : s->last_outage_started_ms,
           s->outage_started_ms != 0 ? now - s->outage_started_ms : s->last_outage_ms,
           s->outage_total_ms, s->outage_started_ms != 0 ? 0 : s->last_outage_ended_ms,
           s->connection_failures, s->query_failures, s->reconnects, b->revision);
    for (size_t i = 0; i < count; ++i) append_record(b, &records[i], i != 0);
    append(b, "],\"alarms\":");
    if (b->snapshot) {
        append(b, "[");
        for (size_t i = 0; i < alarm_count; ++i) append_record(b, &alarms[i], i != 0);
        append(b, "]}");
    } else append(b, "null}");
}

static void failure(struct vigil_queue *q, bool connection, const char error[6])
{
    uint64_t now = vigil_now_ms();
    (void)pthread_mutex_lock(&q->mutex);
    if (connection) ++q->stats.connection_failures;
    else ++q->stats.query_failures;
    q->stats.connected = false;
    memcpy(q->stats.last_error, error, 6);
    if (q->stats.outage_started_ms == 0) {
        q->stats.outage_started_ms = now;
        ++q->stats.outages;
    }
    (void)pthread_mutex_unlock(&q->mutex);
}

static void pause_until(struct vigil_queue *q, uint64_t deadline, bool retry, uint64_t revision)
{
    (void)pthread_mutex_lock(&q->mutex);
    /* Retry wakes must not turn enqueues into a connection busy-loop. */
    for (;;) {
        /* Predicate and wait share the lock: a producer signal arriving before
         * this wait must not leave ready records asleep until the probe tick. */
        if (!retry && (q->stats.stopping || q->stats.reading_depth != 0 ||
                       q->stats.event_depth != 0 || q->alarm_revision != revision)) break;
        uint64_t limit = deadline;
        if (q->stats.stopping && q->stop_deadline_ms < limit) limit = q->stop_deadline_ms;
        if (vigil_now_ms() >= limit) break;
        struct timespec ts = {.tv_sec = (time_t)(limit / 1000U),
                              .tv_nsec = (long)((limit % 1000U) * 1000000U)};
        int result = pthread_cond_timedwait(&q->changed, &q->mutex, &ts);
        if (result == ETIMEDOUT || (!retry && result == 0)) break;
    }
    (void)pthread_mutex_unlock(&q->mutex);
}

static void *writer(void *context)
{
    struct vigil_persistence *p = context;
    struct vigil_queue *q = &p->queue;
    PGconn *conn = NULL;
    struct batch *b = calloc(1, sizeof(*b));
    if (b == NULL) abort();
    bool pending = false, recovery = true;
    uint64_t batch_id = 0, saved_revision = UINT64_MAX, last_write = 0;
    while (!expired(q, UINT64_MAX)) {
        if (conn == NULL) {
            conn = connect_db(p);
            if (conn == NULL) {
                failure(q, true, "08001");
                recovery = true;
                pause_until(q, vigil_now_ms() + RETRY_MS, true, 0);
                continue;
            }
            (void)pthread_mutex_lock(&q->mutex);
            if (q->stats.connections++ != 0) ++q->stats.reconnects;
            (void)pthread_mutex_unlock(&q->mutex);
        }
        if (!pending) {
            struct vigil_queue_stats s;
            vigil_queue_stats(q, &s);
            (void)pthread_mutex_lock(&q->mutex);
            bool changed = q->alarm_revision != saved_revision;
            (void)pthread_mutex_unlock(&q->mutex);
            bool drops = s.readings_dropped != s.acknowledged_reading_drops ||
                         s.events_dropped != s.acknowledged_event_drops;
            if (!recovery && !changed && !drops && s.reading_depth + s.event_depth == 0) {
                if (s.stopping) break;
                if (vigil_now_ms() - last_write < RETRY_MS) {
                    pause_until(q, last_write + RETRY_MS, false, saved_revision);
                    continue;
                }
            }
            make_batch(p, b, ++batch_id, saved_revision, recovery);
            recovery = false;
            pending = true;
        }
        char error[6];
        if (!write_db(q, conn, b->json, error)) {
            failure(q, false, error);
            PQfinish(conn);
            conn = NULL;
            recovery = true;
            /* Retain identical payload and ID even after ambiguous COMMIT. */
            pause_until(q, vigil_now_ms() + RETRY_MS, true, 0);
            continue;
        }
        last_write = vigil_now_ms();
        vigil_queue_ack(q);
        (void)pthread_mutex_lock(&q->mutex);
        struct vigil_queue_stats *s = &q->stats;
        s->connected = true;
        s->acknowledged_revision = b->revision;
        if (s->outage_started_ms != 0) {
            s->last_outage_started_ms = s->outage_started_ms;
            s->last_outage_ended_ms = last_write;
            s->last_outage_ms = last_write - s->outage_started_ms;
            s->outage_total_ms += s->last_outage_ms;
            s->outage_started_ms = 0;
        }
        s->acknowledged_reading_drops = b->captured.readings_dropped;
        s->acknowledged_event_drops = b->captured.events_dropped;
        ++s->batches_written;
        if (b->gap) ++s->gap_markers;
        if (b->snapshot) ++s->snapshots_written;
        (void)pthread_mutex_unlock(&q->mutex);
        saved_revision = b->revision;
        pending = false;
    }
    if (conn != NULL) PQfinish(conn);
    free(b);
    (void)pthread_mutex_lock(&q->mutex);
    q->stats.abandoned_readings = q->stats.reading_depth + q->stats.in_flight_readings;
    q->stats.abandoned_events = q->stats.event_depth + q->stats.in_flight - q->stats.in_flight_readings;
    (void)pthread_mutex_unlock(&q->mutex);
    return NULL;
}

static bool copy_env(char *out, size_t size, const char *name, const char *fallback)
{
    const char *value = getenv(name);
    if (value == NULL) value = fallback;
    if (*value == '\0' || strlen(value) >= size) return false;
    memcpy(out, value, strlen(value) + 1U);
    return true;
}

bool vigil_persistence_start(struct vigil_persistence *p, size_t capacity)
{
    memset(p, 0, sizeof(*p));
    /* Fixed numeric/Unix endpoint avoids DNS blocking inside PQconnectStart.
     * Service/hostaddr indirection would defeat that restriction. */
    if (getenv("PGSERVICE") != NULL || getenv("PGHOSTADDR") != NULL ||
        !copy_env(p->host, sizeof(p->host), "VIGIL_DB_HOST", "/var/run/postgresql") ||
        !copy_env(p->port, sizeof(p->port), "VIGIL_DB_PORT", "5432") ||
        !copy_env(p->database, sizeof(p->database), "VIGIL_DB_NAME", "vigil") ||
        !copy_env(p->user, sizeof(p->user), "VIGIL_DB_USER", "vigil")) return false;
    for (size_t i = 0; p->port[i] != '\0'; ++i)
        if (p->port[i] < '0' || p->port[i] > '9') return false;
    unsigned long port = strtoul(p->port, NULL, 10);
    if (port == 0 || port > 65535UL) return false;
    if (strchr(p->host, ',') != NULL) return false; /* disallow libpq multi-host/DNS indirection */
    if (p->host[0] != '/') {
        unsigned char address[16];
        if (inet_pton(AF_INET, p->host, address) != 1 &&
            inet_pton(AF_INET6, p->host, address) != 1) return false;
        p->network = true;
    }
    unsigned char id[16];
    if (getrandom(id, sizeof(id), 0) != (ssize_t)sizeof(id)) return false;
    id[6] = (unsigned char)((id[6] & 15U) | 64U);
    id[8] = (unsigned char)((id[8] & 63U) | 128U);
    (void)snprintf(p->run_id, sizeof(p->run_id),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        (unsigned)id[0], (unsigned)id[1], (unsigned)id[2], (unsigned)id[3],
        (unsigned)id[4], (unsigned)id[5], (unsigned)id[6], (unsigned)id[7],
        (unsigned)id[8], (unsigned)id[9], (unsigned)id[10], (unsigned)id[11],
        (unsigned)id[12], (unsigned)id[13], (unsigned)id[14], (unsigned)id[15]);
    p->started_ms = vigil_now_ms();
    p->started_utc_ms = vigil_utc_ms();
    if (!vigil_queue_init(&p->queue, capacity)) return false;
    if (pthread_create(&p->thread, NULL, writer, p) != 0) {
        vigil_queue_destroy(&p->queue);
        return false;
    }
    return true;
}

void vigil_persistence_stop(struct vigil_persistence *p)
{
    uint64_t deadline = vigil_now_ms() + DRAIN_MS;
    (void)pthread_mutex_lock(&p->queue.mutex);
    p->queue.stats.stopping = true;
    p->queue.stop_deadline_ms = deadline;
    (void)pthread_cond_signal(&p->queue.changed);
    (void)pthread_mutex_unlock(&p->queue.mutex);
    (void)pthread_join(p->thread, NULL);
}
void vigil_persistence_destroy(struct vigil_persistence *p)
{
    vigil_queue_destroy(&p->queue);
}
