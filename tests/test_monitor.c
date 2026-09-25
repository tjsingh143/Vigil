#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "vigil/monitor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Expectations: docs/CHECKPOINT_2_VERIFICATION.md. All times are injected;
 * no sockets, sleeping, wall-clock dependency or database in these tests. */
struct fixture { struct vigil_config config; struct vigil_monitor monitor; };

static void init(struct fixture *f)
{
    vigil_config_defaults(&f->config);
    assert_true(vigil_monitor_init(&f->monitor, &f->config, 100, NULL, NULL));
}

static bool rd(struct fixture *f, uint8_t pod, uint8_t channel, int16_t value,
                 uint8_t status, uint8_t counter, uint64_t now)
{
    uint16_t wire = (uint16_t)value;
    uint8_t bytes[] = {channel, (uint8_t)wire, (uint8_t)(wire >> 8U), status, counter, 0};
    assert_true(vigil_crc8(bytes, 5, &bytes[5]));
    return vigil_monitor_frame(&f->monitor, 0x100U + pod, false, bytes, sizeof(bytes), now);
}

static bool hb(struct fixture *f, uint8_t pod, uint8_t state, uint8_t counter, uint64_t now)
{
    uint8_t bytes[] = {state, 42, 0, 0, 0, counter, 0};
    assert_true(vigil_crc8(bytes, 6, &bytes[6]));
    return vigil_monitor_frame(&f->monitor, 0x700U + pod, false, bytes, sizeof(bytes), now);
}

static void balance(const struct fixture *f)
{
    const struct vigil_monitor_counters *c = &f->monitor.counters;
    uint64_t rejected = 0;
    for (size_t i = 0; i < VIGIL_REJECT_COUNT; ++i) rejected += c->rejected[i];
    assert_int_equal(rejected, c->frames_rejected);
    assert_int_equal(c->frames_received, rejected + c->valid_readings + c->heartbeats);
}

static void test_initialization_reset(void **state)
{
    (void)state;
    struct fixture f;
    init(&f);
    assert_false(vigil_source_current(&f.monitor.pods[0].source[0]));
    assert_int_equal(f.monitor.counters.alarm_transitions, 0);
    vigil_monitor_tick(&f.monitor, 3099);
    assert_int_equal(f.monitor.counters.stale_events, 0);
    vigil_monitor_tick(&f.monitor, 3100);
    assert_int_equal(f.monitor.counters.stale_events, 3);
    assert_false(f.monitor.pods[0].source[0].has_value);
    assert_true(f.monitor.pods[0].source[0].stale);
    assert_true(vigil_monitor_init(&f.monitor, &f.monitor.config, 4000, NULL, NULL));
    assert_int_equal(f.monitor.counters.stale_events, 0);
    assert_false(f.monitor.pods[0].source[0].stale);
    assert_false(f.monitor.pods[0].source[0].communication.active);
    assert_false(f.monitor.pods[0].readings.initialized);
    assert_int_equal(f.monitor.pods[0].source[0].high.severity, VIGIL_CRITICAL);
}

static void test_progression(void **state)
{
    (void)state;
    struct fixture f;
    init(&f);
    assert_true(rd(&f, 1, 0, 2200, 0, 200, 100));
    assert_true(rd(&f, 1, 1, 0, 0, 201, 100));
    assert_true(hb(&f, 1, 1, 55, 100));
    assert_true(rd(&f, 1, 0, 2201, 0, 202, 200));
    assert_true(hb(&f, 1, 1, 56, 200));
    assert_int_equal(f.monitor.counters.valid_readings, 3);
    assert_int_equal(f.monitor.counters.heartbeats, 2);
    assert_int_equal(f.monitor.counters.inferred_loss, 0);
    assert_int_equal(f.monitor.pods[0].readings.counter, 202);
    assert_int_equal(f.monitor.pods[0].heartbeat.counter, 56);
    assert_true(vigil_source_current(&f.monitor.pods[0].source[0]));
    balance(&f);
}

static void test_repeat_and_old(void **state)
{
    (void)state;
    struct fixture f;
    init(&f);
    assert_true(rd(&f, 1, 0, 2200, 0, 10, 100));
    struct vigil_source saved = f.monitor.pods[0].source[0];
    assert_false(rd(&f, 1, 0, 3000, 0, 10, 200));
    assert_false(rd(&f, 1, 0, 3000, 0, 9, 300));
    assert_false(rd(&f, 1, 0, 3000, 0, 138, 400));
    assert_memory_equal(&saved, &f.monitor.pods[0].source[0], sizeof(saved));
    assert_int_equal(f.monitor.counters.rejected[VIGIL_REJECT_REPEAT], 1);
    assert_int_equal(f.monitor.counters.rejected[VIGIL_REJECT_OLD], 2);
    assert_int_equal(f.monitor.pods[0].readings.counter, 10);
    assert_true(f.monitor.pods[0].readings.sequence.active);
    assert_int_equal(f.monitor.pods[0].readings.sequence.transitions, 1);
    assert_true(rd(&f, 1, 0, 2200, 0, 11, 500));
    assert_false(f.monitor.pods[0].readings.sequence.active);
    assert_int_equal(f.monitor.pods[0].readings.sequence.cleared_ms, 500);
    balance(&f);
}

static void test_gap_acceptance(void **state)
{
    (void)state;
    struct fixture f;
    init(&f);
    assert_true(rd(&f, 1, 0, 2200, 0, 10, 100));
    assert_true(rd(&f, 1, 0, 2201, 0, 14, 200));
    assert_int_equal(f.monitor.pods[0].source[0].value, 2201);
    assert_int_equal(f.monitor.counters.inferred_loss, 3);
    assert_true(f.monitor.pods[0].readings.sequence.active);
    assert_true(rd(&f, 1, 1, 0, 0, 15, 200));
    assert_false(f.monitor.pods[0].readings.sequence.active);
    balance(&f);
}

static void test_wraparound(void **state)
{
    (void)state;
    struct fixture f;
    init(&f);
    const uint8_t counters[] = {254, 255, 0, 3};
    for (size_t i = 0; i < sizeof(counters); ++i)
        assert_true(rd(&f, 1, 0, 2200, 0, counters[i], 100U + i));
    assert_int_equal(f.monitor.counters.inferred_loss, 2);
    assert_int_equal(f.monitor.counters.valid_readings, 4);
    balance(&f);
}

static void seed(struct fixture *f)
{
    assert_true(rd(f, 1, 0, 2200, 0, 30, 100));
    assert_true(rd(f, 1, 1, 0, 0, 31, 100));
    assert_true(hb(f, 1, 1, 30, 100));
}

static void test_timeout_once(void **state)
{
    (void)state;
    struct fixture f;
    init(&f);
    seed(&f);
    vigil_monitor_tick(&f.monitor, 3099);
    assert_true(vigil_source_current(&f.monitor.pods[0].source[0]));
    vigil_monitor_tick(&f.monitor, 3100);
    assert_int_equal(f.monitor.counters.stale_events, 3);
    assert_false(vigil_source_current(&f.monitor.pods[0].source[0]));
    assert_int_equal(f.monitor.pods[0].source[0].value, 2200);
    assert_int_equal(f.monitor.pods[0].source[0].communication.raised_ms, 3100);
    vigil_monitor_tick(&f.monitor, 9000);
    assert_int_equal(f.monitor.counters.stale_events, 3);
    assert_int_equal(f.monitor.counters.alarm_transitions, 3);
}

static void test_silence_recovery(void **state)
{
    (void)state;
    struct fixture f;
    init(&f);
    seed(&f);
    vigil_monitor_tick(&f.monitor, 3100);
    assert_true(hb(&f, 1, 1, 0, 3101));
    assert_true(f.monitor.pods[0].source[0].stale);
    assert_true(rd(&f, 1, 0, 2200, 0, 0, 3101));
    assert_true(rd(&f, 1, 1, 0, 0, 1, 3101));
    assert_int_equal(f.monitor.counters.counter_resets, 2);
    assert_int_equal(f.monitor.counters.recoveries, 3);
    assert_int_equal(f.monitor.counters.inferred_loss, 0);
    assert_false(f.monitor.pods[0].source[0].communication.active);
    assert_int_equal(f.monitor.pods[0].source[0].communication.cleared_ms, 3101);
    assert_true(vigil_source_current(&f.monitor.pods[0].source[0]));
    balance(&f);
}

static void test_continuous_repeats_stay_stale(void **state)
{
    (void)state;
    struct fixture f;
    init(&f);
    seed(&f);
    for (uint64_t now = 1100; now <= 10100; now += 1000) {
        vigil_monitor_tick(&f.monitor, now);
        assert_false(rd(&f, 1, 1, 1, 0, 31, now));
        assert_false(hb(&f, 1, 1, 30, now));
    }
    assert_int_equal(f.monitor.counters.counter_resets, 0);
    assert_int_equal(f.monitor.counters.rejected[VIGIL_REJECT_REPEAT], 20);
    assert_int_equal(f.monitor.counters.valid_readings, 2);
    assert_true(f.monitor.pods[0].source[1].stale);
    assert_true(f.monitor.pods[0].heartbeat_stale);
    balance(&f);
}

static void test_heartbeat_only(void **state)
{
    (void)state;
    struct fixture f;
    init(&f);
    seed(&f);
    for (uint8_t i = 1; i <= 4; ++i) {
        uint64_t now = 100U + (uint64_t)i * 1000U;
        vigil_monitor_tick(&f.monitor, now);
        assert_true(hb(&f, 1, 1, (uint8_t)(30U + i), now));
    }
    assert_true(f.monitor.pods[0].source[0].stale);
    assert_true(f.monitor.pods[0].source[1].stale);
    assert_false(f.monitor.pods[0].heartbeat_stale);
    assert_int_equal(f.monitor.counters.stale_events, 2);
}

static void test_range_and_fault(void **state)
{
    (void)state;
    struct fixture f;
    init(&f);
    assert_true(rd(&f, 1, 0, 2200, 0, 0, 100));
    assert_false(rd(&f, 1, 0, 8501, 0, 1, 200));
    struct vigil_source *s = &f.monitor.pods[0].source[0];
    assert_int_equal(s->value, 2200);
    assert_int_equal(s->last_valid_ms, 100);
    assert_int_equal(s->validity, VIGIL_RANGE);
    assert_false(vigil_source_current(s));
    assert_true(s->invalid.active);
    assert_false(rd(&f, 1, 0, 2200, 1, 2, 300));
    assert_int_equal(s->validity, VIGIL_FAULT);
    assert_true(rd(&f, 1, 0, 2201, 0, 3, 400));
    assert_false(s->invalid.active);
    assert_true(vigil_source_current(s));
    assert_int_equal(f.monitor.counters.inferred_loss, 0);
    assert_int_equal(f.monitor.counters.rejected[VIGIL_REJECT_RANGE], 1);
    assert_int_equal(f.monitor.counters.rejected[VIGIL_REJECT_FAULT], 1);
    balance(&f);
}

static void test_range_boundaries(void **state)
{
    (void)state;
    struct fixture f;
    init(&f);
    f.config.pods[0].source[0].rate_per_s = 0;
    assert_true(vigil_monitor_init(&f.monitor, &f.config, 100, NULL, NULL));
    assert_false(rd(&f, 1, 0, -4001, 0, 0, 100));
    assert_true(rd(&f, 1, 0, -4000, 0, 1, 100));
    assert_true(rd(&f, 1, 0, 8500, 0, 2, 100));
    assert_false(rd(&f, 1, 0, 8501, 0, 3, 100));
    assert_int_equal(f.monitor.counters.rejected[VIGIL_REJECT_RANGE], 2);
    balance(&f);
}

static void test_rate_boundaries(void **state)
{
    (void)state;
    struct fixture f;
    init(&f);
    assert_true(rd(&f, 1, 0, 2200, 0, 0, 100));
    assert_false(rd(&f, 1, 0, 2201, 0, 1, 100)); /* dt=0, changed */
    assert_true(rd(&f, 1, 0, 2200, 0, 2, 100));  /* dt=0, equal */
    assert_false(rd(&f, 1, 0, 2701, 0, 3, 1100));
    assert_int_equal(f.monitor.pods[0].source[0].last_valid_ms, 100);
    assert_true(rd(&f, 1, 0, 2700, 0, 4, 1100)); /* exact 500 units/s */
    assert_false(rd(&f, 1, 0, 2199, 0, 5, 2100));
    assert_true(rd(&f, 1, 0, 2200, 0, 6, 2100)); /* negative slope */
    assert_int_equal(f.monitor.counters.rejected[VIGIL_REJECT_RATE], 3);
    assert_int_equal(f.monitor.counters.inferred_loss, 0);
    balance(&f);
}

static void test_rate_rebaseline_after_stale(void **state)
{
    (void)state;
    struct fixture f;
    init(&f);
    assert_true(rd(&f, 1, 0, -4000, 0, 0, 100));
    assert_false(rd(&f, 1, 0, 8500, 0, 1, 3099));
    vigil_monitor_tick(&f.monitor, 3100);
    assert_true(rd(&f, 1, 0, 8500, 0, 2, 3100));
    assert_true(vigil_source_current(&f.monitor.pods[0].source[0]));
    assert_int_equal(f.monitor.counters.counter_resets, 0); /* invalid frame was traffic */
    balance(&f);
}

static void test_flat_suspicion(void **state)
{
    (void)state;
    struct fixture f;
    init(&f);
    f.config.pods[0].source[0].flat_ms = 2000;
    assert_true(vigil_monitor_init(&f.monitor, &f.config, 100, NULL, NULL));
    assert_true(rd(&f, 1, 0, 2200, 0, 0, 100));
    assert_true(rd(&f, 1, 0, 2200, 0, 1, 2099));
    assert_false(f.monitor.pods[0].source[0].flat.active);
    assert_true(rd(&f, 1, 0, 2200, 0, 2, 2100));
    assert_true(f.monitor.pods[0].source[0].flat.active);
    assert_int_equal(f.monitor.pods[0].source[0].flat.severity, VIGIL_WARNING);
    assert_int_equal(f.monitor.pods[0].source[0].validity, VIGIL_VALID);
    assert_true(vigil_source_current(&f.monitor.pods[0].source[0]));
    assert_false(f.monitor.pods[0].source[0].invalid.active);
    assert_true(rd(&f, 1, 0, 2201, 0, 3, 2200));
    assert_false(f.monitor.pods[0].source[0].flat.active);
    assert_int_equal(f.monitor.counters.frames_rejected, 0);
}

static void test_flat_gaps_and_invalid(void **state)
{
    (void)state;
    struct fixture f;
    init(&f);
    f.config.pods[0].source[0].flat_ms = 1000;
    assert_true(vigil_monitor_init(&f.monitor, &f.config, 100, NULL, NULL));
    assert_true(rd(&f, 1, 0, 2200, 0, 0, 100));
    vigil_monitor_tick(&f.monitor, 1100);
    assert_false(f.monitor.pods[0].source[0].flat.active); /* silence alone */
    assert_true(rd(&f, 1, 0, 2200, 0, 1, 1100));
    assert_true(f.monitor.pods[0].source[0].flat.active);
    assert_false(rd(&f, 1, 0, 2200, 1, 2, 1200));
    assert_false(f.monitor.pods[0].source[0].flat.active);
    assert_true(rd(&f, 1, 0, 2200, 0, 3, 1300));
    assert_false(f.monitor.pods[0].source[0].flat.active);
    assert_true(rd(&f, 1, 0, 2200, 0, 4, 2300));
    vigil_monitor_tick(&f.monitor, 5300);
    assert_false(f.monitor.pods[0].source[0].flat.active);
    assert_true(rd(&f, 1, 0, 2200, 0, 0, 5300));
    assert_false(f.monitor.pods[0].source[0].flat.active);
}

static void test_hysteresis(void **state)
{
    (void)state;
    struct fixture f;
    init(&f);
    f.config.pods[0].source[0].rate_per_s = 0;
    assert_true(vigil_monitor_init(&f.monitor, &f.config, 100, NULL, NULL));
    const int16_t values[] = {2699, 2700, 2699, 2701, 2601, 2690, 2600, 2699};
    const bool active[] = {false, true, true, true, true, true, false, false};
    for (uint8_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        assert_true(rd(&f, 1, 0, values[i], 0, i, 100U + i));
        assert_int_equal(f.monitor.pods[0].source[0].high.active, active[i]);
    }
    const struct vigil_alarm *a = &f.monitor.pods[0].source[0].high;
    assert_int_equal(a->transitions, 2);
    assert_int_equal(a->raised_ms, 101);
    assert_int_equal(a->cleared_ms, 106);
}

static void test_invalid_stale_cannot_clear_high(void **state)
{
    (void)state;
    struct fixture f;
    init(&f);
    assert_true(rd(&f, 1, 0, 2800, 0, 0, 100));
    assert_false(rd(&f, 1, 0, 2500, 1, 1, 200));
    assert_true(f.monitor.pods[0].source[0].high.active);
    vigil_monitor_tick(&f.monitor, 3100);
    assert_true(f.monitor.pods[0].source[0].high.active);
    assert_false(vigil_source_current(&f.monitor.pods[0].source[0]));
}

static void test_multiple_sources(void **state)
{
    (void)state;
    struct fixture f;
    init(&f);
    f.config.pod_count = 2;
    f.config.pods[1] = f.config.pods[0];
    f.config.pods[1].id = 2;
    f.config.pods[1].source[0].high = 2100;
    f.config.pods[1].source[0].clear = 2000;
    assert_true(vigil_monitor_init(&f.monitor, &f.config, 100, NULL, NULL));
    seed(&f);
    assert_true(rd(&f, 2, 0, 2200, 0, 30, 100));
    assert_true(rd(&f, 2, 1, 1, 0, 31, 100));
    assert_true(hb(&f, 2, 1, 30, 100));
    assert_false(f.monitor.pods[0].source[0].high.active);
    assert_false(f.monitor.pods[0].source[1].high.active);
    assert_true(f.monitor.pods[1].source[0].high.active);
    assert_true(f.monitor.pods[1].source[1].high.active);
    assert_true(rd(&f, 1, 1, 0, 0, 32, 3000));
    vigil_monitor_tick(&f.monitor, 3100);
    assert_true(f.monitor.pods[0].source[0].stale);
    assert_false(f.monitor.pods[0].source[1].stale);
    assert_true(f.monitor.pods[1].source[1].stale);
    balance(&f);
}

static void test_heartbeat_state(void **state)
{
    (void)state;
    struct fixture f;
    init(&f);
    assert_true(hb(&f, 1, 0, 0, 100));
    assert_true(f.monitor.pods[0].state_alarm.active);
    assert_true(hb(&f, 1, 2, 1, 200));
    assert_int_equal(f.monitor.pods[0].state_alarm.transitions, 1);
    assert_false(hb(&f, 1, 1, 1, 300));
    assert_int_equal(f.monitor.pods[0].state, 2);
    assert_true(hb(&f, 1, 1, 2, 400));
    assert_false(f.monitor.pods[0].state_alarm.active);
    assert_int_equal(f.monitor.pods[0].uptime_s, 42);
    balance(&f);
}

static void test_malformed_immutable(void **state)
{
    (void)state;
    struct fixture f;
    init(&f);
    seed(&f);
    struct vigil_pod saved = f.monitor.pods[0];
    uint8_t bytes[] = {0, 0x98, 8, 0, 32, 0};
    assert_true(vigil_crc8(bytes, 5, &bytes[5]));
    assert_false(vigil_monitor_frame(&f.monitor, 0x101, false, bytes, 1, 200));
    assert_false(vigil_monitor_frame(&f.monitor, 0x100, false, bytes, 6, 200));
    assert_false(vigil_monitor_frame(&f.monitor, 0x101, true, bytes, 6, 200));
    bytes[5] ^= 1U;
    assert_false(vigil_monitor_frame(&f.monitor, 0x101, false, bytes, 6, 200));
    assert_memory_equal(&saved, &f.monitor.pods[0], sizeof(saved));
    assert_int_equal(f.monitor.counters.alarm_transitions, 0);
    assert_int_equal(f.monitor.counters.rejected[VIGIL_REJECT_LENGTH], 1);
    assert_int_equal(f.monitor.counters.rejected[VIGIL_REJECT_ID], 1);
    assert_int_equal(f.monitor.counters.rejected[VIGIL_REJECT_FORMAT], 1);
    assert_int_equal(f.monitor.counters.rejected[VIGIL_REJECT_CRC], 1);
    balance(&f);
}

static void test_unconfigured(void **state)
{
    (void)state;
    struct fixture f;
    init(&f);
    f.config.pods[0].source[1].enabled = false;
    assert_true(vigil_monitor_init(&f.monitor, &f.config, 100, NULL, NULL));
    assert_false(rd(&f, 2, 0, 2200, 0, 0, 100));
    assert_true(rd(&f, 1, 0, 2200, 0, 0, 100));
    assert_false(rd(&f, 1, 1, 0, 0, 1, 100));
    assert_true(rd(&f, 1, 0, 2200, 0, 2, 100));
    assert_int_equal(f.monitor.pods[0].readings.counter, 2);
    assert_int_equal(f.monitor.counters.inferred_loss, 0);
    vigil_monitor_tick(&f.monitor, 3100);
    assert_int_equal(f.monitor.counters.stale_events, 2);
    assert_false(f.monitor.pods[0].source[1].communication.active);
    balance(&f);
}

static void count_callback(void *context, uint8_t pod, int channel,
                             const struct vigil_alarm *alarm, uint64_t now)
{
    unsigned *calls = context;
    ++*calls;
    assert_int_equal(pod, 1);
    assert_int_equal(channel, 1);
    assert_int_equal(alarm->kind, VIGIL_ALARM_HIGH);
    assert_int_equal(alarm->active ? alarm->raised_ms : alarm->cleared_ms, now);
}

static void test_callback_and_door(void **state)
{
    (void)state;
    struct fixture f;
    init(&f);
    unsigned calls = 0;
    assert_true(vigil_monitor_init(&f.monitor, &f.config, 100, count_callback, &calls));
    assert_true(rd(&f, 1, 1, 1, 0, 0, 100));
    assert_true(rd(&f, 1, 1, 1, 0, 1, 200));
    assert_true(rd(&f, 1, 1, 0, 0, 2, 300));
    assert_int_equal(calls, 2);
    assert_false(f.monitor.pods[0].source[1].flat.active);
}

static bool load_text(const char *text, struct vigil_config *config)
{
    char path[] = "/tmp/vigil-config-XXXXXX";
    int fd = mkstemp(path);
    assert_true(fd >= 0);
    assert_int_equal(write(fd, text, strlen(text)), strlen(text));
    assert_int_equal(close(fd), 0);
    char error[128] = {0};
    bool result = vigil_config_load(path, config, error, sizeof(error));
    assert_int_equal(unlink(path), 0);
    if (!result) assert_true(error[0] != '\0');
    return result;
}

static void test_config_valid(void **state)
{
    (void)state;
    struct vigil_config config;
    assert_true(load_text("# comment\ntick_ms 20\nstatus_ms 0\npod 7 500\n"
        "source 7 temperature 400 -5000 9000 100 2000 2800 2500 # C\n"
        "pod 127 1000\nsource 127 door 1000 0 1 0 0 1 0\n", &config));
    assert_int_equal(config.pod_count, 2);
    assert_int_equal(config.pods[0].id, 7);
    assert_int_equal(config.pods[0].source[0].minimum, -5000);
    assert_int_equal(config.pods[1].source[1].high, 1);
    assert_int_equal(config.status_ms, 0);
    assert_false(config.pods[0].source[1].enabled);
}

static void test_config_rejects(void **state)
{
    (void)state;
    const char *bad[] = {"", "pod 1 3000\n", "pot 1 3000\n",
        "pod 1 3000 extra\n", "pod 1 -1\n", "pod 1 99999999999999999999999\n",
        "pod 0 3000\n", "pod 128 3000\n", "pod 1 3000\npod 1 3000\n",
        "source 1 door 3000 0 1 0 0 1 0\n", "tick_ms 20\ntick_ms 30\n",
        "status_ms 0\nstatus_ms 0\n", "pod 1 3000\nsource 1 temp 3000 0 1 0 0 1 0\n",
        "pod 1 3000\nsource 1 door 3000 0 1 0 0 1 0\nsource 1 door 3000 0 1 0 0 1 0\n",
        "pod 1 3000\nsource 1 door 3000 0 1 0 0 1 1\n",
        "pod 1 3000\nsource 1 door 3000 0 2 0 0 1 0\n",
        "pod 1 3000\nsource 1 door 3000 0 1 0 0 1 0 extra\n",
        "pod 1 3000\nsource 1 temperature 3000 -32769 1 0 0 1 0\n",
        "pod 1 3000\nsource 1 temperature 3000 0 32768 0 0 1 0\n",
        "pod 1 3000\nsource 1 temperature 3000 -4000 8500 -1 0 2700 2600\n",
        "tick_ms 0\npod 1 3000\nsource 1 door 3000 0 1 0 0 1 0\n",
        "pod 1 3000\nsource 1 door 50 0 1 0 0 1 0\n"};
    struct vigil_config config, saved;
    vigil_config_defaults(&config);
    saved = config;
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        assert_false(load_text(bad[i], &config));
        assert_memory_equal(&config, &saved, sizeof(config));
    }
    char long_line[600];
    memset(long_line, '#', sizeof(long_line) - 1U);
    long_line[sizeof(long_line) - 1U] = '\0';
    assert_false(load_text(long_line, &config));
    char error[64];
    assert_false(vigil_config_load("/no/such/vigil/config", &config, error, sizeof(error)));
}

static void test_invalid_init_immutable(void **state)
{
    (void)state;
    struct fixture f;
    init(&f);
    seed(&f);
    struct vigil_monitor saved = f.monitor;
    f.config.pod_count = VIGIL_MAX_PODS + 1U;
    assert_false(vigil_monitor_init(&f.monitor, &f.config, 100, NULL, NULL));
    assert_memory_equal(&saved, &f.monitor, sizeof(saved));
    assert_false(vigil_config_valid(NULL));
}

static void test_names(void **state)
{
    (void)state;
    for (int r = 0; r < VIGIL_REJECT_COUNT; ++r)
        assert_string_not_equal(vigil_reject_name((enum vigil_reject)r), "unknown");
    assert_string_equal(vigil_alarm_name(VIGIL_ALARM_FLAT), "flat_suspicion");
    assert_string_equal(vigil_reject_name((enum vigil_reject)-1), "unknown");
    assert_string_equal(vigil_alarm_name((enum vigil_alarm_kind)-1), "unknown");
    assert_string_equal(vigil_validity_name((enum vigil_validity)-1), "unknown");
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_initialization_reset), cmocka_unit_test(test_progression),
        cmocka_unit_test(test_repeat_and_old), cmocka_unit_test(test_gap_acceptance),
        cmocka_unit_test(test_wraparound), cmocka_unit_test(test_timeout_once),
        cmocka_unit_test(test_silence_recovery), cmocka_unit_test(test_continuous_repeats_stay_stale),
        cmocka_unit_test(test_heartbeat_only), cmocka_unit_test(test_range_and_fault),
        cmocka_unit_test(test_range_boundaries), cmocka_unit_test(test_rate_boundaries),
        cmocka_unit_test(test_rate_rebaseline_after_stale), cmocka_unit_test(test_flat_suspicion),
        cmocka_unit_test(test_flat_gaps_and_invalid), cmocka_unit_test(test_hysteresis),
        cmocka_unit_test(test_invalid_stale_cannot_clear_high), cmocka_unit_test(test_multiple_sources),
        cmocka_unit_test(test_heartbeat_state), cmocka_unit_test(test_malformed_immutable),
        cmocka_unit_test(test_unconfigured), cmocka_unit_test(test_callback_and_door),
        cmocka_unit_test(test_config_valid), cmocka_unit_test(test_config_rejects),
        cmocka_unit_test(test_invalid_init_immutable), cmocka_unit_test(test_names)
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
