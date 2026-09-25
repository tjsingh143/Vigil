#include "vigil/monitor.h"

#include <string.h>

static struct vigil_alarm alarm_new(enum vigil_alarm_kind kind,
                                     enum vigil_severity severity)
{
    return (struct vigil_alarm){.kind = kind, .severity = severity};
}

static void alarm_set(struct vigil_monitor *m, struct vigil_alarm *alarm,
                       bool active, uint8_t pod, int channel, uint64_t now)
{
    if (alarm->active == active) return;
    alarm->active = active;
    if (active) alarm->raised_ms = now;
    else alarm->cleared_ms = now;
    ++alarm->transitions;
    ++m->counters.alarm_transitions;
    if (m->on_alarm != NULL)
        m->on_alarm(m->callback_context, pod, channel, alarm, now);
}

bool vigil_monitor_init(struct vigil_monitor *m, const struct vigil_config *config,
                         uint64_t now, vigil_alarm_callback callback, void *context)
{
    if (m == NULL || !vigil_config_valid(config)) return false;
    /* Copy first so resetting with &monitor.config is well-defined. */
    struct vigil_config saved = *config;
    memset(m, 0, sizeof(*m));
    m->config = saved;
    m->started_ms = now;
    m->on_alarm = callback;
    m->callback_context = context;
    for (size_t p = 0; p < saved.pod_count; ++p) {
        struct vigil_pod *pod = &m->pods[p];
        pod->communication = alarm_new(VIGIL_ALARM_COMMUNICATION, VIGIL_CRITICAL);
        pod->state_alarm = alarm_new(VIGIL_ALARM_POD_STATE, VIGIL_WARNING);
        pod->readings.sequence = alarm_new(VIGIL_ALARM_SEQUENCE, VIGIL_WARNING);
        pod->heartbeat.sequence = alarm_new(VIGIL_ALARM_SEQUENCE, VIGIL_WARNING);
        for (size_t c = 0; c < VIGIL_CHANNELS; ++c) {
            struct vigil_source *s = &pod->source[c];
            s->communication = alarm_new(VIGIL_ALARM_COMMUNICATION, VIGIL_CRITICAL);
            s->invalid = alarm_new(VIGIL_ALARM_INVALID, VIGIL_WARNING);
            s->flat = alarm_new(VIGIL_ALARM_FLAT, VIGIL_WARNING);
            s->high = alarm_new(VIGIL_ALARM_HIGH, VIGIL_CRITICAL);
        }
    }
    return true;
}

bool vigil_source_current(const struct vigil_source *s)
{
    return s->has_value && !s->stale && s->validity == VIGIL_VALID;
}

void vigil_monitor_tick(struct vigil_monitor *m, uint64_t now)
{
    for (size_t p = 0; p < m->config.pod_count; ++p) {
        const struct vigil_pod_config *pc = &m->config.pods[p];
        struct vigil_pod *pod = &m->pods[p];
        uint64_t last = pod->has_heartbeat ? pod->last_heartbeat_ms : m->started_ms;
        if (!pod->heartbeat_stale && now - last >= pc->heartbeat_ms) {
            pod->heartbeat_stale = true;
            ++m->counters.stale_events;
            alarm_set(m, &pod->communication, true, pc->id, -1, now);
        }
        for (size_t c = 0; c < VIGIL_CHANNELS; ++c) {
            if (!pc->source[c].enabled) continue;
            struct vigil_source *s = &pod->source[c];
            last = s->has_value ? s->last_valid_ms : m->started_ms;
            if (!s->stale && now - last >= pc->source[c].timeout_ms) {
                s->stale = true;
                s->flat_tracking = false;
                ++m->counters.stale_events;
                alarm_set(m, &s->communication, true, pc->id, (int)c, now);
                /* A gap ends the observation window, not proof of flatness. */
                alarm_set(m, &s->flat, false, pc->id, (int)c, now);
            }
        }
    }
}

static bool reject(struct vigil_monitor *m, enum vigil_reject reason)
{
    ++m->counters.frames_rejected;
    ++m->counters.rejected[reason];
    return false;
}

static bool progress(struct vigil_monitor *m, struct vigil_stream *stream,
                       uint8_t counter, uint64_t timeout, uint8_t pod,
                       int channel, uint64_t now)
{
    if (stream->initialized && now - stream->last_seen_ms >= timeout) {
        stream->initialized = false;
        ++stream->resets;
        ++m->counters.counter_resets;
    }
    stream->last_seen_ms = now;
    struct vigil_counter_result seq = vigil_counter_check(stream->initialized,
                                                           stream->counter, counter);
    alarm_set(m, &stream->sequence, !seq.accept || seq.lost != 0,
              pod, channel, now);
    if (!seq.accept) {
        if (seq.sequence == VIGIL_SEQ_REPEAT) {
            ++stream->repeats;
            return reject(m, VIGIL_REJECT_REPEAT);
        }
        ++stream->old;
        return reject(m, VIGIL_REJECT_OLD);
    }
    stream->initialized = true;
    stream->counter = counter;
    ++stream->accepted;
    stream->inferred_loss += seq.lost;
    m->counters.inferred_loss += seq.lost;
    return true;
}

static bool reading(struct vigil_monitor *m, size_t p,
                      const struct vigil_message *msg, uint64_t now)
{
    const struct vigil_pod_config *pc = &m->config.pods[p];
    const struct vigil_source_config *sc = &pc->source[msg->channel];
    struct vigil_source *s = &m->pods[p].source[msg->channel];
    enum vigil_validity validity = VIGIL_VALID;
    enum vigil_reject reason = VIGIL_REJECT_FAULT;
    if (msg->status == VIGIL_STATUS_FAULT) validity = VIGIL_FAULT;
    else if (msg->value < sc->minimum || msg->value > sc->maximum) {
        validity = VIGIL_RANGE;
        reason = VIGIL_REJECT_RANGE;
    } else if (sc->rate_per_s != 0 && s->has_value &&
               now - s->last_valid_ms < sc->timeout_ms) {
        int32_t delta = (int32_t)msg->value - (int32_t)s->value;
        uint64_t magnitude = (uint64_t)(delta < 0 ? -delta : delta);
        /* Bounded integer operands: delta <= 65535, dt < 86400000 ms. */
        if (magnitude * 1000U > (uint64_t)sc->rate_per_s * (now - s->last_valid_ms)) {
            validity = VIGIL_RATE;
            reason = VIGIL_REJECT_RATE;
        }
    }
    s->validity = validity;
    alarm_set(m, &s->invalid, validity != VIGIL_VALID, pc->id, msg->channel, now);
    if (validity != VIGIL_VALID) {
        s->flat_tracking = false;
        alarm_set(m, &s->flat, false, pc->id, msg->channel, now);
        return reject(m, reason);
    }
    if (!s->flat_tracking || !s->has_value || s->value != msg->value ||
        now - s->last_valid_ms >= sc->timeout_ms) {
        s->flat_since_ms = now;
        s->flat_tracking = true;
    }
    alarm_set(m, &s->flat, sc->flat_ms != 0 && now - s->flat_since_ms >= sc->flat_ms,
              pc->id, msg->channel, now);
    s->value = msg->value;
    s->has_value = true;
    s->last_valid_ms = now;
    ++s->accepted;
    ++m->counters.valid_readings;
    if (s->stale) ++m->counters.recoveries;
    s->stale = false;
    alarm_set(m, &s->communication, false, pc->id, msg->channel, now);
    if (!s->high.active && msg->value >= sc->high)
        alarm_set(m, &s->high, true, pc->id, msg->channel, now);
    else if (s->high.active && msg->value <= sc->clear)
        alarm_set(m, &s->high, false, pc->id, msg->channel, now);
    return true;
}

bool vigil_monitor_frame(struct vigil_monitor *m, uint32_t raw_id, bool is_fd,
                          const uint8_t *data, size_t length, uint64_t now)
{
    ++m->counters.frames_received;
    struct vigil_message msg;
    enum vigil_result result = vigil_decode(raw_id, is_fd, data, length, &msg);
    if (result != VIGIL_OK) {
        /* Explicit mapping keeps wire and monitoring reason enums independent. */
        static const enum vigil_reject reasons[VIGIL_RESULT_COUNT] = {
            VIGIL_REJECT_ARGUMENT, VIGIL_REJECT_ARGUMENT, VIGIL_REJECT_FORMAT,
            VIGIL_REJECT_ID, VIGIL_REJECT_LENGTH, VIGIL_REJECT_CRC,
            VIGIL_REJECT_CHANNEL, VIGIL_REJECT_STATUS, VIGIL_REJECT_VALUE,
            VIGIL_REJECT_STATE};
        return reject(m, reasons[result]);
    }
    size_t p = 0;
    while (p < m->config.pod_count && m->config.pods[p].id != msg.pod_id) ++p;
    if (p == m->config.pod_count) return reject(m, VIGIL_REJECT_POD);
    const struct vigil_pod_config *pc = &m->config.pods[p];
    struct vigil_pod *pod = &m->pods[p];
    if (msg.kind == VIGIL_READING) {
        uint64_t timeout = 0;
        for (size_t c = 0; c < VIGIL_CHANNELS; ++c)
            if (pc->source[c].enabled && pc->source[c].timeout_ms > timeout)
                timeout = pc->source[c].timeout_ms;
        if (!progress(m, &pod->readings, msg.counter, timeout, pc->id, -2, now)) return false;
        /* Both channels share the stream even when only one is monitored.
         * Advance sequence state for an excluded channel to avoid false loss. */
        if (!pc->source[msg.channel].enabled) return reject(m, VIGIL_REJECT_SOURCE);
        return reading(m, p, &msg, now);
    }
    if (!progress(m, &pod->heartbeat, msg.counter, pc->heartbeat_ms, pc->id, -1, now))
        return false;
    pod->has_heartbeat = true;
    pod->last_heartbeat_ms = now;
    pod->state = msg.pod_state;
    pod->uptime_s = msg.uptime_s;
    if (pod->heartbeat_stale) ++m->counters.recoveries;
    pod->heartbeat_stale = false;
    ++m->counters.heartbeats;
    alarm_set(m, &pod->communication, false, pc->id, -1, now);
    alarm_set(m, &pod->state_alarm, msg.pod_state != VIGIL_POD_OPERATIONAL,
              pc->id, -1, now);
    return true;
}

const char *vigil_reject_name(enum vigil_reject reason)
{
    static const char *const names[] = {"argument", "format", "id", "length",
        "crc", "channel", "status", "value", "state", "unconfigured_pod",
        "unconfigured_source", "repeat", "old", "sensor_fault", "range", "rate"};
    return (unsigned)reason < VIGIL_REJECT_COUNT ? names[reason] : "unknown";
}

const char *vigil_alarm_name(enum vigil_alarm_kind kind)
{
    static const char *const names[] = {"communication_lost", "sequence_anomaly",
        "measurement_invalid", "flat_suspicion", "high", "pod_not_operational"};
    return (unsigned)kind < sizeof(names) / sizeof(names[0]) ? names[kind] : "unknown";
}

const char *vigil_validity_name(enum vigil_validity validity)
{
    static const char *const names[] = {"never", "valid", "sensor_fault", "range", "rate"};
    return (unsigned)validity < sizeof(names) / sizeof(names[0]) ? names[validity] : "unknown";
}
