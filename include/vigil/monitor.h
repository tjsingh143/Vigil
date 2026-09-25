#ifndef VIGIL_MONITOR_H
#define VIGIL_MONITOR_H

#include "vigil/protocol.h"

#define VIGIL_MAX_PODS 16U
#define VIGIL_CHANNELS 2U

struct vigil_source_config {
    bool enabled;
    uint64_t timeout_ms;
    int16_t minimum, maximum;
    uint32_t rate_per_s; /* wire units/second; zero disables */
    uint64_t flat_ms;    /* equal accepted values; zero disables */
    int16_t high, clear; /* inclusive raise/clear, clear < high */
};
struct vigil_pod_config {
    uint8_t id;
    uint64_t heartbeat_ms;
    struct vigil_source_config source[VIGIL_CHANNELS];
};
struct vigil_config {
    uint64_t tick_ms, status_ms;
    size_t pod_count;
    struct vigil_pod_config pods[VIGIL_MAX_PODS];
};
void vigil_config_defaults(struct vigil_config *config);
bool vigil_config_valid(const struct vigil_config *config);
/* Transactional: leave out unchanged on failure. Syntax errors include line number. */
bool vigil_config_load(const char *path, struct vigil_config *out,
                       char *error, size_t error_size);

enum vigil_alarm_kind {
    VIGIL_ALARM_COMMUNICATION, VIGIL_ALARM_SEQUENCE,
    VIGIL_ALARM_INVALID, VIGIL_ALARM_FLAT, VIGIL_ALARM_HIGH,
    VIGIL_ALARM_POD_STATE
};
enum vigil_severity { VIGIL_WARNING, VIGIL_CRITICAL };
struct vigil_alarm {
    enum vigil_alarm_kind kind;
    enum vigil_severity severity;
    bool active;
    uint64_t raised_ms, cleared_ms, transitions;
};
/* channel -1 denotes the heartbeat, -2 the shared reading stream.
 * Callback is synchronous; it must not block or reenter the monitor. */
typedef void (*vigil_alarm_callback)(void *context, uint8_t pod, int channel,
                                      const struct vigil_alarm *alarm,
                                      uint64_t now_ms);

enum vigil_reject {
    VIGIL_REJECT_ARGUMENT, VIGIL_REJECT_FORMAT, VIGIL_REJECT_ID,
    VIGIL_REJECT_LENGTH, VIGIL_REJECT_CRC, VIGIL_REJECT_CHANNEL,
    VIGIL_REJECT_STATUS, VIGIL_REJECT_VALUE, VIGIL_REJECT_STATE,
    VIGIL_REJECT_POD, VIGIL_REJECT_SOURCE,
    VIGIL_REJECT_REPEAT, VIGIL_REJECT_OLD,
    VIGIL_REJECT_FAULT, VIGIL_REJECT_RANGE, VIGIL_REJECT_RATE,
    VIGIL_REJECT_COUNT
};
struct vigil_monitor_counters {
    uint64_t frames_received, frames_rejected;
    uint64_t rejected[VIGIL_REJECT_COUNT];
    uint64_t valid_readings, heartbeats, inferred_loss;
    uint64_t stale_events, recoveries, alarm_transitions, counter_resets;
};
enum vigil_validity { VIGIL_NO_VALUE, VIGIL_VALID, VIGIL_FAULT,
                      VIGIL_RANGE, VIGIL_RATE };
struct vigil_stream {
    bool initialized;
    uint8_t counter;
    uint64_t last_seen_ms; /* decodable traffic, including rejected sequences */
    uint64_t accepted, repeats, old, inferred_loss, resets;
    struct vigil_alarm sequence;
};
struct vigil_source {
    bool has_value, stale;
    int16_t value; /* last accepted usable value, never overwritten on rejection */
    enum vigil_validity validity; /* latest progressing measurement */
    uint64_t last_valid_ms, accepted;
    bool flat_tracking;
    uint64_t flat_since_ms;
    struct vigil_alarm communication, invalid, flat, high;
};
struct vigil_pod {
    struct vigil_stream readings, heartbeat;
    bool has_heartbeat, heartbeat_stale;
    uint8_t state;
    uint32_t uptime_s;
    uint64_t last_heartbeat_ms;
    struct vigil_alarm communication, state_alarm;
    struct vigil_source source[VIGIL_CHANNELS];
};
struct vigil_monitor {
    struct vigil_config config; /* owned value copy, no config pointers */
    uint64_t started_ms;
    struct vigil_monitor_counters counters; /* persistence gets its own group later */
    struct vigil_pod pods[VIGIL_MAX_PODS];
    vigil_alarm_callback on_alarm;
    void *callback_context;
};
/* Single owner, no I/O/heap/clock calls. now_ms must be nondecreasing monotonic
 * milliseconds across tick/frame calls. Tick before frames and periodically
 * during silence to expire freshness. Init is also a full runtime reset. */
bool vigil_monitor_init(struct vigil_monitor *monitor,
                         const struct vigil_config *config, uint64_t now_ms,
                         vigil_alarm_callback callback, void *context);
void vigil_monitor_tick(struct vigil_monitor *monitor, uint64_t now_ms);
/* Returns true for accepted usable readings or progressing heartbeats.
 * Malformed frames change counters only, never source/alarm state. */
bool vigil_monitor_frame(struct vigil_monitor *monitor, uint32_t raw_id,
                          bool is_fd, const uint8_t *data, size_t length,
                          uint64_t now_ms);
bool vigil_source_current(const struct vigil_source *source);
const char *vigil_reject_name(enum vigil_reject reason);
const char *vigil_alarm_name(enum vigil_alarm_kind kind);
const char *vigil_validity_name(enum vigil_validity validity);
#endif
