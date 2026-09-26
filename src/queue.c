#include "vigil/queue.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t clock_ms(clockid_t clock)
{
    struct timespec ts;
    /* Linux clocks are required infrastructure; never continue with bogus time. */
    if (clock_gettime(clock, &ts) != 0 || ts.tv_sec < 0) abort();
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}
uint64_t vigil_now_ms(void) { return clock_ms(CLOCK_MONOTONIC); }
uint64_t vigil_utc_ms(void) { return clock_ms(CLOCK_REALTIME); }

bool vigil_queue_init(struct vigil_queue *q, size_t capacity)
{
    if (q == NULL || capacity == 0 || capacity > 65536U) return false;
    memset(q, 0, sizeof(*q));
    q->readings = calloc(capacity, sizeof(*q->readings));
    if (q->readings == NULL) return false;
    if (pthread_mutex_init(&q->mutex, NULL) != 0) { free(q->readings); return false; }
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0) goto fail_mutex;
    int result = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (result == 0) result = pthread_cond_init(&q->changed, &attr);
    (void)pthread_condattr_destroy(&attr);
    if (result != 0) goto fail_mutex;
    q->stats.capacity = capacity;
    return true;
fail_mutex:
    (void)pthread_mutex_destroy(&q->mutex);
    free(q->readings);
    return false;
}

void vigil_queue_destroy(struct vigil_queue *q)
{
    (void)pthread_cond_destroy(&q->changed);
    (void)pthread_mutex_destroy(&q->mutex);
    free(q->readings);
    q->readings = NULL;
}

void vigil_queue_push(struct vigil_queue *q, struct vigil_record record)
{
    (void)pthread_mutex_lock(&q->mutex);
    struct vigil_queue_stats *s = &q->stats;
    if (s->stopping) { (void)pthread_mutex_unlock(&q->mutex); return; }
    struct vigil_record *lane = record.reading ? q->readings : q->events;
    size_t *head = record.reading ? &q->reading_head : &q->event_head;
    size_t *depth = record.reading ? &s->reading_depth : &s->event_depth;
    size_t *high = record.reading ? &s->reading_high_water : &s->event_high_water;
    size_t capacity = record.reading ? s->capacity : VIGIL_EVENT_CAPACITY;
    if (record.reading) ++s->readings_queued;
    else ++s->events_queued;
    if (*depth == capacity) {
        if (s->readings_dropped + s->events_dropped == 0 ||
            lane[*head].mono_ms < s->first_drop_ms) s->first_drop_ms = lane[*head].mono_ms;
        if (lane[*head].mono_ms > s->last_drop_ms) s->last_drop_ms = lane[*head].mono_ms;
        if (record.reading) ++s->readings_dropped;
        else ++s->events_dropped;
        *head = (*head + 1U) % capacity;
        --*depth;
    }
    record.seq = ++q->next_seq;
    lane[(*head + *depth) % capacity] = record;
    ++*depth;
    if (*depth > *high) *high = *depth;
    (void)pthread_cond_signal(&q->changed);
    (void)pthread_mutex_unlock(&q->mutex);
}

size_t vigil_queue_take(struct vigil_queue *q, struct vigil_record out[VIGIL_BATCH_SIZE])
{
    (void)pthread_mutex_lock(&q->mutex);
    struct vigil_queue_stats *s = &q->stats;
    size_t n = 0;
    /* Exactly one immutable in-flight batch: never replace an ambiguous commit. */
    if (s->in_flight == 0) {
        while (n < VIGIL_BATCH_SIZE && (s->reading_depth != 0 || s->event_depth != 0)) {
            bool reading = s->reading_depth != 0 && (s->event_depth == 0 ||
                q->readings[q->reading_head].seq < q->events[q->event_head].seq);
            if (reading) {
                out[n++] = q->readings[q->reading_head];
                q->reading_head = (q->reading_head + 1U) % s->capacity;
                --s->reading_depth;
                ++s->in_flight_readings;
            } else {
                out[n++] = q->events[q->event_head];
                q->event_head = (q->event_head + 1U) % VIGIL_EVENT_CAPACITY;
                --s->event_depth;
            }
        }
        s->in_flight = n;
    }
    (void)pthread_mutex_unlock(&q->mutex);
    return n;
}

void vigil_queue_ack(struct vigil_queue *q)
{
    (void)pthread_mutex_lock(&q->mutex);
    q->stats.records_written += q->stats.in_flight;
    q->stats.readings_written += q->stats.in_flight_readings;
    q->stats.in_flight = 0;
    q->stats.in_flight_readings = 0;
    (void)pthread_mutex_unlock(&q->mutex);
}

void vigil_queue_stats(struct vigil_queue *q, struct vigil_queue_stats *out)
{
    (void)pthread_mutex_lock(&q->mutex);
    *out = q->stats;
    (void)pthread_mutex_unlock(&q->mutex);
}

void vigil_queue_publish(struct vigil_queue *q, const struct vigil_record *alarms,
                         size_t count, uint64_t revision)
{
    if (count > VIGIL_ALARM_CAPACITY) abort();
    (void)pthread_mutex_lock(&q->mutex);
    memcpy(q->alarms, alarms, count * sizeof(*alarms));
    q->alarm_count = count;
    q->alarm_revision = revision;
    (void)pthread_cond_signal(&q->changed);
    (void)pthread_mutex_unlock(&q->mutex);
}

bool vigil_persistence_degraded(struct vigil_persistence_health *h,
    const struct vigil_queue_stats *s, bool was_degraded, uint64_t published_revision)
{
    bool new_loss = s->readings_dropped != h->reading_drops_seen ||
                    s->events_dropped != h->event_drops_seen;
    h->reading_drops_seen = s->readings_dropped;
    h->event_drops_seen = s->events_dropped;
    return new_loss || !s->connected ||
           s->readings_dropped != s->acknowledged_reading_drops ||
           s->events_dropped != s->acknowledged_event_drops ||
           (was_degraded && (s->reading_depth + s->event_depth + s->in_flight != 0 ||
                            s->acknowledged_revision < published_revision));
}
