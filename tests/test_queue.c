#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include "vigil/queue.h"
#include <sched.h>
#include <stdio.h>
#include <string.h>

static struct vigil_record record(bool reading, uint64_t time)
{
    struct vigil_record r = {.reading = reading, .mono_ms = time};
    (void)snprintf(r.kind, sizeof(r.kind), "%s", reading ? "reading" : "alarm");
    (void)snprintf(r.detail, sizeof(r.detail), "{\"time\":%llu}", (unsigned long long)time);
    return r;
}
static void limits(void **state)
{
    (void)state;
    struct vigil_queue q;
    assert_false(vigil_queue_init(&q, 0));
    assert_false(vigil_queue_init(&q, 65537));
    assert_true(vigil_queue_init(&q, 1));
    struct vigil_record out[VIGIL_BATCH_SIZE];
    assert_int_equal(vigil_queue_take(&q, out), 0);
    vigil_queue_destroy(&q);
}
static void oldest_and_ownership(void **state)
{
    (void)state;
    struct vigil_queue q;
    assert_true(vigil_queue_init(&q, 3));
    for (uint64_t i = 1; i <= 8; ++i) {
        struct vigil_record r = record(true, i);
        vigil_queue_push(&q, r);
        memset(&r, 0, sizeof(r));
    }
    struct vigil_queue_stats s;
    vigil_queue_stats(&q, &s);
    assert_int_equal(s.readings_queued, 8);
    assert_int_equal(s.readings_dropped, 5);
    assert_int_equal(s.first_drop_ms, 1);
    assert_int_equal(s.last_drop_ms, 5);
    assert_int_equal(s.reading_high_water, 3);
    struct vigil_record out[VIGIL_BATCH_SIZE];
    assert_int_equal(vigil_queue_take(&q, out), 3);
    for (size_t i = 0; i < 3; ++i) assert_int_equal(out[i].mono_ms, i + 6);
    assert_string_equal(out[0].detail, "{\"time\":6}");
    assert_int_equal(vigil_queue_take(&q, out), 0); /* pinned until ack */
    vigil_queue_ack(&q);
    vigil_queue_stats(&q, &s);
    assert_int_equal(s.readings_written, 3);
    assert_int_equal(s.in_flight, 0);
    vigil_queue_destroy(&q);
}
static void independent_lanes_and_partial(void **state)
{
    (void)state;
    struct vigil_queue q;
    assert_true(vigil_queue_init(&q, 2));
    vigil_queue_push(&q, record(true, 1));
    for (uint64_t i = 0; i < VIGIL_EVENT_CAPACITY + 4U; ++i)
        vigil_queue_push(&q, record(false, i + 2));
    vigil_queue_push(&q, record(true, 999));
    struct vigil_queue_stats s;
    vigil_queue_stats(&q, &s);
    assert_int_equal(s.readings_dropped, 0);
    assert_int_equal(s.events_dropped, 4);
    assert_int_equal(s.event_high_water, VIGIL_EVENT_CAPACITY);
    struct vigil_record out[VIGIL_BATCH_SIZE];
    uint64_t previous = 0;
    size_t count, total = 0, last_count = 0;
    while ((count = vigil_queue_take(&q, out)) != 0) {
        for (size_t i = 0; i < count; ++i) {
            assert_true(out[i].seq > previous);
            previous = out[i].seq;
        }
        last_count = count;
        total += count;
        vigil_queue_ack(&q);
    }
    assert_int_equal(last_count, 2);
    assert_int_equal(total, 258);
    vigil_queue_stats(&q, &s);
    assert_int_equal(s.records_written, 258);
    assert_int_equal(s.readings_written, 2);
    vigil_queue_destroy(&q);
}
static void in_flight_and_snapshot(void **state)
{
    (void)state;
    struct vigil_queue q;
    assert_true(vigil_queue_init(&q, 2));
    vigil_queue_push(&q, record(true, 1));
    struct vigil_record out[VIGIL_BATCH_SIZE];
    assert_int_equal(vigil_queue_take(&q, out), 1);
    for (uint64_t i = 2; i <= 5; ++i) vigil_queue_push(&q, record(true, i));
    assert_int_equal(out[0].mono_ms, 1); /* retries retain copied payload */
    struct vigil_queue_stats s;
    vigil_queue_stats(&q, &s);
    assert_int_equal(s.in_flight_readings, 1);
    assert_int_equal(s.readings_dropped, 2);
    assert_int_equal(s.readings_queued, s.readings_written + s.readings_dropped +
                                      s.reading_depth + s.in_flight_readings);
    vigil_queue_publish(&q, out, 1, 7);
    memset(out, 0, sizeof(out));
    assert_int_equal(q.alarms[0].mono_ms, 1);
    assert_int_equal(q.alarm_revision, 7);
    vigil_queue_ack(&q);
    vigil_queue_destroy(&q);
}
struct concurrent { struct vigil_queue q; bool ordered; };
static void *consume(void *context)
{
    struct concurrent *c = context;
    struct vigil_record out[VIGIL_BATCH_SIZE];
    uint64_t previous = 0;
    c->ordered = true;
    for (;;) {
        size_t n = vigil_queue_take(&c->q, out);
        for (size_t i = 0; i < n; ++i) {
            if (out[i].seq <= previous || out[i].seq != out[i].mono_ms + 1U) c->ordered = false;
            previous = out[i].seq;
        }
        if (n != 0) vigil_queue_ack(&c->q);
        struct vigil_queue_stats s;
        vigil_queue_stats(&c->q, &s);
        if (s.stopping && s.reading_depth + s.event_depth == 0) break;
        (void)sched_yield();
    }
    return NULL;
}
static void concurrency(void **state)
{
    (void)state;
    struct concurrent c;
    assert_true(vigil_queue_init(&c.q, 17));
    pthread_t thread;
    assert_int_equal(pthread_create(&thread, NULL, consume, &c), 0);
    for (uint64_t i = 0; i < 20000; ++i) vigil_queue_push(&c.q, record(i % 2 == 0, i));
    (void)pthread_mutex_lock(&c.q.mutex);
    c.q.stats.stopping = true;
    (void)pthread_mutex_unlock(&c.q.mutex);
    assert_int_equal(pthread_join(thread, NULL), 0);
    struct vigil_queue_stats s;
    vigil_queue_stats(&c.q, &s);
    assert_true(c.ordered);
    assert_int_equal(s.readings_queued, 10000);
    assert_int_equal(s.events_queued, 10000);
    assert_int_equal(s.readings_queued, s.readings_written + s.readings_dropped);
    assert_int_equal(s.records_written + s.readings_dropped + s.events_dropped, 20000);
    assert_int_equal(s.reading_depth + s.event_depth + s.in_flight, 0);
    vigil_queue_push(&c.q, record(true, 99999)); /* stopped queue rejects new ownership */
    vigil_queue_stats(&c.q, &s);
    assert_int_equal(s.readings_queued, 10000);
    vigil_queue_destroy(&c.q);
}
static void degradation_policy(void **state)
{
    (void)state;
    struct vigil_persistence_health h = {0};
    struct vigil_queue_stats s = {0};
    assert_true(vigil_persistence_degraded(&h, &s, false, 1)); /* unavailable */
    s.connected = true;
    s.acknowledged_revision = 1;
    assert_false(vigil_persistence_degraded(&h, &s, true, 1)); /* caught up */
    s.reading_depth = 1;
    assert_false(vigil_persistence_degraded(&h, &s, false, 1)); /* normal buffering */
    assert_true(vigil_persistence_degraded(&h, &s, true, 1)); /* latched until drained */
    s.reading_depth = 0;
    assert_true(vigil_persistence_degraded(&h, &s, true, 2)); /* snapshot not yet saved */
    s.acknowledged_revision = 2;
    s.readings_dropped = s.acknowledged_reading_drops = 5;
    /* Writer can acknowledge loss before main observes it: still raise once. */
    assert_true(vigil_persistence_degraded(&h, &s, false, 2));
    assert_false(vigil_persistence_degraded(&h, &s, true, 2));
    s.events_dropped = 3;
    assert_true(vigil_persistence_degraded(&h, &s, false, 2));
    assert_true(vigil_persistence_degraded(&h, &s, true, 2));
    s.acknowledged_event_drops = 3;
    assert_false(vigil_persistence_degraded(&h, &s, true, 2));
    s.local_mode = s.local_ready = true;
    s.durable_batches = 1;
    /* Healthy outbox buffering, including the persisted clear transition,
     * cannot itself re-raise the alarm and create an endless event loop. */
    assert_false(vigil_persistence_degraded(&h, &s, false, 2));
    assert_true(vigil_persistence_degraded(&h, &s, true, 2));
    s.durable_batches = 0;
    assert_false(vigil_persistence_degraded(&h, &s, true, 2));
    s.local_state = 1;
    assert_true(vigil_persistence_degraded(&h, &s, false, 2));
}
int main(void)
{
    const struct CMUnitTest tests[] = {cmocka_unit_test(limits),
        cmocka_unit_test(oldest_and_ownership), cmocka_unit_test(independent_lanes_and_partial),
        cmocka_unit_test(in_flight_and_snapshot), cmocka_unit_test(concurrency),
        cmocka_unit_test(degradation_policy)};
    return cmocka_run_group_tests(tests, NULL, NULL);
}
