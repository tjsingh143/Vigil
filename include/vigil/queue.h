#ifndef VIGIL_QUEUE_H
#define VIGIL_QUEUE_H
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VIGIL_EVENT_CAPACITY 256U
#define VIGIL_BATCH_SIZE 32U
#define VIGIL_ALARM_CAPACITY 193U /* 16 pods * 12 alarms + persistence */
#define VIGIL_DETAIL_SIZE 384U
struct vigil_record {
    uint64_t seq, mono_ms, utc_ms;
    unsigned pod;
    int channel;
    bool reading;
    char kind[32];
    char detail[VIGIL_DETAIL_SIZE]; /* locally generated JSON, value owned */
};
struct vigil_queue_stats {
    uint64_t readings_queued, events_queued, records_written, readings_written;
    uint64_t readings_dropped, events_dropped, first_drop_ms, last_drop_ms;
    uint64_t readings_evicted, events_evicted, readings_rejected, events_rejected;
    uint64_t closed_readings, closed_events;
    uint64_t locally_committed, locally_committed_readings, local_failures;
    uint64_t durable_records, durable_batches, durable_bytes, spool_admitted, spool_retired;
    uint64_t local_revision, local_reading_drops, local_event_drops;
    unsigned local_state;
    int sqlite_error;
    bool local_mode, local_ready, count_exact;
    uint64_t acknowledged_reading_drops, acknowledged_event_drops;
    uint64_t connection_failures, query_failures, connections, reconnects;
    uint64_t outage_started_ms, outage_total_ms, last_outage_ms, outages;
    uint64_t last_outage_started_ms, last_outage_ended_ms;
    uint64_t batches_written, gap_markers, snapshots_written, acknowledged_revision;
    char last_error[6]; /* SQLSTATE, or 08001/08006 for transport failures */
    uint64_t abandoned_readings, abandoned_events;
    size_t reading_depth, event_depth, reading_high_water, event_high_water;
    size_t capacity, in_flight, in_flight_readings;
    bool connected, stopping;
};
struct vigil_queue {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    struct vigil_record *readings;
    struct vigil_record events[VIGIL_EVENT_CAPACITY];
    size_t reading_head, event_head;
    uint64_t next_seq;
    struct vigil_queue_stats stats;
    struct vigil_record alarms[VIGIL_ALARM_CAPACITY];
    size_t alarm_count;
    uint64_t alarm_revision;
    uint64_t stop_deadline_ms;
};
/* Producer and consumer own their input/output values. No borrowed payloads.
 * All shared fields are accessed under mutex; no I/O in these operations. */
bool vigil_queue_init(struct vigil_queue *q, size_t capacity);
void vigil_queue_destroy(struct vigil_queue *q); /* after writer joined */
enum vigil_queue_result { VIGIL_QUEUED, VIGIL_QUEUED_EVICTED, VIGIL_CLOSED, VIGIL_RESOURCE_REJECTED };
enum vigil_queue_result vigil_queue_push(struct vigil_queue *q, struct vigil_record record);
struct vigil_capture;
void vigil_queue_take_local(struct vigil_queue *q, struct vigil_capture *capture);
size_t vigil_queue_take(struct vigil_queue *q, struct vigil_record out[VIGIL_BATCH_SIZE]);
void vigil_queue_ack(struct vigil_queue *q);
void vigil_queue_stats(struct vigil_queue *q, struct vigil_queue_stats *out);
void vigil_queue_publish(struct vigil_queue *q, const struct vigil_record *alarms,
                         size_t count, uint64_t revision);
/* Pure main-owned policy: observe new loss even if writer already recorded it. */
struct vigil_persistence_health { uint64_t reading_drops_seen, event_drops_seen; };
bool vigil_persistence_degraded(struct vigil_persistence_health *health,
    const struct vigil_queue_stats *stats, bool was_degraded, uint64_t published_revision);
/* Shared-state lock wrappers also enforce that this caller holds no queue
 * lock at a storage I/O boundary (other threads may independently enqueue). */
void vigil_queue_lock(struct vigil_queue *q);
void vigil_queue_unlock(struct vigil_queue *q);
void vigil_queue_assert_io(void);
uint64_t vigil_now_ms(void);
uint64_t vigil_utc_ms(void);
#endif
