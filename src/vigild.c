#include "vigil/monitor.h"
#include "vigil/persistence.h"
#include "vigil/outbox.h"
#include "vigil/replay.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <systemd/sd-daemon.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

struct diagnostics {
    uint64_t dropped, snapshots, rx_overflow, missed_ticks;
    uint32_t last_rx_overflow;
    struct vigil_persistence *persistence;
    struct vigil_alarm degraded;
    struct vigil_persistence_health health;
    uint64_t persisted_revision, degraded_transitions;
};

static void emit(struct diagnostics *d, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

static void emit(struct diagnostics *d, const char *format, ...)
{
    /* One <= PIPE_BUF nonblocking write per line, no unbounded output queue. */
    char line[2048];
    va_list args;
    va_start(args, format);
    int size = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (size < 0 || (size_t)size >= sizeof(line)) { ++d->dropped; return; }
    ssize_t written;
    do { written = write(STDOUT_FILENO, line, (size_t)size); }
    while (written < 0 && errno == EINTR);
    if (written != size) ++d->dropped;
}

static bool monotonic_ms(uint64_t *out)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0 || time.tv_sec < 0) return false;
    *out = (uint64_t)time.tv_sec * 1000U + (uint64_t)time.tv_nsec / 1000000U;
    return true;
}

static void copy_alarm(struct vigil_record *r, unsigned pod, int channel,
                       const struct vigil_alarm *a, uint64_t now)
{
    *r = (struct vigil_record){.pod = pod, .channel = channel,
        .mono_ms = now, .utc_ms = vigil_utc_ms(), .kind = "alarm"};
    (void)snprintf(r->detail, sizeof(r->detail),
        "{\"kind\":\"%s\",\"active\":%s,\"severity\":\"%s\","
        "\"raised_ms\":%" PRIu64 ",\"cleared_ms\":%" PRIu64 ",\"transitions\":%" PRIu64 "}",
        vigil_alarm_name(a->kind), a->active ? "true" : "false",
        a->severity == VIGIL_CRITICAL ? "critical" : "warning",
        a->raised_ms, a->cleared_ms, a->transitions);
}

static void alarm_event(void *context, uint8_t pod, int channel,
                         const struct vigil_alarm *alarm, uint64_t now)
{
    struct diagnostics *d = context;
    if (d->persistence != NULL) {
        struct vigil_record r;
        copy_alarm(&r, pod, channel, alarm, now);
        vigil_queue_push(&d->persistence->queue, r);
    }
    emit(d, "alarm now_ms=%" PRIu64 " pod=%u channel=%d kind=%s state=%s severity=%s"
            " raised_ms=%" PRIu64 " cleared_ms=%" PRIu64 " transitions=%" PRIu64 "\n",
         now, (unsigned)pod, channel, vigil_alarm_name(alarm->kind),
         alarm->active ? "active" : "cleared",
         alarm->severity == VIGIL_CRITICAL ? "critical" : "warning",
         alarm->raised_ms, alarm->cleared_ms, alarm->transitions);
}

static void alarm_status(struct diagnostics *d, uint8_t pod, int channel,
                          const struct vigil_alarm *alarm)
{
    emit(d, "alarm_status snapshot=%" PRIu64 " pod=%u channel=%d kind=%s active=%u"
            " severity=%s raised_ms=%" PRIu64 " cleared_ms=%" PRIu64
            " transitions=%" PRIu64 "\n", d->snapshots, (unsigned)pod, channel,
         vigil_alarm_name(alarm->kind), (unsigned)alarm->active,
         alarm->severity == VIGIL_CRITICAL ? "critical" : "warning",
         alarm->raised_ms, alarm->cleared_ms, alarm->transitions);
}

static const char *freshness(bool seen, bool stale)
{
    return stale ? "stale" : seen ? "fresh" : "never";
}

static void stream_status(struct diagnostics *d, uint8_t pod, int channel,
                           const struct vigil_stream *s)
{
    emit(d, "progress snapshot=%" PRIu64 " pod=%u channel=%d initialized=%u"
            " counter=%u last_seen_ms=%" PRIu64 " accepted=%" PRIu64
            " repeats=%" PRIu64 " old=%" PRIu64 " loss=%" PRIu64
            " resets=%" PRIu64 "\n", d->snapshots, (unsigned)pod, channel,
         (unsigned)s->initialized, (unsigned)s->counter, s->last_seen_ms,
         s->accepted, s->repeats, s->old, s->inferred_loss, s->resets);
    alarm_status(d, pod, channel, &s->sequence);
}

static void snapshot(struct diagnostics *d, const struct vigil_monitor *m, uint64_t now)
{
    const struct vigil_monitor_counters *c = &m->counters;
    ++d->snapshots;
    emit(d, "snapshot id=%" PRIu64 " now_ms=%" PRIu64 " started_ms=%" PRIu64
            " frames=%" PRIu64 " rejected=%" PRIu64 " valid_readings=%" PRIu64
            " heartbeats=%" PRIu64 " loss=%" PRIu64 " stale_events=%" PRIu64
            " recoveries=%" PRIu64 " alarm_transitions=%" PRIu64
            " counter_resets=%" PRIu64 " diagnostics_dropped=%" PRIu64
            " rx_overflow=%" PRIu64 " missed_ticks=%" PRIu64 "\n",
         d->snapshots, now, m->started_ms, c->frames_received, c->frames_rejected,
         c->valid_readings, c->heartbeats, c->inferred_loss, c->stale_events,
         c->recoveries, c->alarm_transitions, c->counter_resets, d->dropped,
         d->rx_overflow, d->missed_ticks);
    for (int reason = 0; reason < VIGIL_REJECT_COUNT; ++reason)
        emit(d, "reject_count snapshot=%" PRIu64 " reason=%s count=%" PRIu64 "\n",
             d->snapshots, vigil_reject_name((enum vigil_reject)reason), c->rejected[reason]);
    for (size_t p = 0; p < m->config.pod_count; ++p) {
        const struct vigil_pod_config *pc = &m->config.pods[p];
        const struct vigil_pod *pod = &m->pods[p];
        emit(d, "pod snapshot=%" PRIu64 " pod=%u heartbeat=%s seen=%u state=%u"
                " uptime_s=%" PRIu32 " last_heartbeat_ms=%" PRIu64 "\n",
             d->snapshots, (unsigned)pc->id,
             freshness(pod->has_heartbeat, pod->heartbeat_stale),
             (unsigned)pod->has_heartbeat, (unsigned)pod->state, pod->uptime_s,
             pod->last_heartbeat_ms);
        alarm_status(d, pc->id, -1, &pod->communication);
        alarm_status(d, pc->id, -1, &pod->state_alarm);
        stream_status(d, pc->id, -1, &pod->heartbeat);
        stream_status(d, pc->id, -2, &pod->readings);
        for (size_t ch = 0; ch < VIGIL_CHANNELS; ++ch) {
            if (!pc->source[ch].enabled) continue;
            const struct vigil_source *s = &pod->source[ch];
            emit(d, "source snapshot=%" PRIu64 " pod=%u channel=%zu freshness=%s"
                    " validity=%s current=%u has_value=%u value=%d last_valid_ms=%" PRIu64
                    " accepted=%" PRIu64 " flat_suspected=%u high=%u\n",
                 d->snapshots, (unsigned)pc->id, ch, freshness(s->has_value, s->stale),
                 vigil_validity_name(s->validity), (unsigned)vigil_source_current(s),
                 (unsigned)s->has_value, (int)s->value, s->last_valid_ms, s->accepted,
                 (unsigned)s->flat.active, (unsigned)s->high.active);
            alarm_status(d, pc->id, (int)ch, &s->communication);
            alarm_status(d, pc->id, (int)ch, &s->invalid);
            alarm_status(d, pc->id, (int)ch, &s->flat);
            alarm_status(d, pc->id, (int)ch, &s->high);
        }
    }
    emit(d, "snapshot_end id=%" PRIu64 " diagnostics_dropped=%" PRIu64 "\n",
         d->snapshots, d->dropped);
}

static void persistence_status(struct diagnostics *d, uint64_t now)
{
    if (d->persistence == NULL) return;
    struct vigil_queue_stats s;
    vigil_queue_stats(&d->persistence->queue, &s);
    /* Writer timestamps can be newer than the poll turn's original sample. */
    now = vigil_now_ms();
    emit(d, "persistence run_id=%s now_ms=%" PRIu64 " connected=%u degraded=%u"
        " degraded_transitions=%" PRIu64 " readings_queued=%" PRIu64
        " events_queued=%" PRIu64 " records_written=%" PRIu64 " readings_written=%" PRIu64
        " readings_dropped=%" PRIu64 " events_dropped=%" PRIu64
        " reading_depth=%zu event_depth=%zu capacity=%zu event_capacity=%u"
        " in_flight=%zu in_flight_readings=%zu high_water=%zu event_high_water=%zu"
        " connection_failures=%" PRIu64 " query_failures=%" PRIu64
        " connections=%" PRIu64 " reconnects=%" PRIu64 " outages=%" PRIu64
        " outage_ms=%" PRIu64 " outage_total_ms=%" PRIu64 " last_outage_ms=%" PRIu64
        " gap_markers=%" PRIu64 " snapshots_written=%" PRIu64 " last_error=%s"
        " alarm_revision=%" PRIu64 " persisted_revision=%" PRIu64
        " abandoned_readings=%" PRIu64 " abandoned_events=%" PRIu64 "\n",
        d->persistence->run_id, now, (unsigned)s.connected, (unsigned)d->degraded.active,
        d->degraded_transitions, s.readings_queued, s.events_queued, s.records_written,
        s.readings_written, s.readings_dropped, s.events_dropped, s.reading_depth,
        s.event_depth, s.capacity, VIGIL_EVENT_CAPACITY, s.in_flight, s.in_flight_readings,
        s.reading_high_water, s.event_high_water, s.connection_failures, s.query_failures,
        s.connections, s.reconnects, s.outages,
        s.outage_started_ms != 0 ? now - s.outage_started_ms : 0,
        s.outage_total_ms, s.last_outage_ms, s.gap_markers, s.snapshots_written, s.last_error[0] == '\0' ? "none" : s.last_error,
        d->persisted_revision, s.acknowledged_revision, s.abandoned_readings, s.abandoned_events);
    emit(d, "outbox run_id=%s local_state=%s remote_state=%s local_committed=%" PRIu64
        " local_readings=%" PRIu64 " durable_pending=%" PRIu64 " pending_batches=%" PRIu64
        " pending_bytes=%" PRIu64 " spool_admitted=%" PRIu64 " spool_retired=%" PRIu64
        " local_failures=%" PRIu64 " sqlite_error=%d reading_evicted=%" PRIu64
        " event_evicted=%" PRIu64 " reading_rejected=%" PRIu64 " event_rejected=%" PRIu64
        " local_revision=%" PRIu64 " replay_attempts=%" PRIu64 " oldest_order=%" PRIu64 " count_exact=%u\n", d->persistence->run_id,
        vigil_store_name((enum vigil_store_result)s.local_state),vigil_remote_name(s.remote_state),s.locally_committed,s.locally_committed_readings,
        s.durable_records,s.durable_batches,s.durable_bytes,s.spool_admitted,s.spool_retired,
        s.local_failures,s.sqlite_error,s.readings_evicted,s.events_evicted,s.readings_rejected,s.events_rejected,
        s.local_revision,s.replay_attempts,s.oldest_pending_order,(unsigned)s.count_exact);
}

static void persistence_update(struct diagnostics *d, const struct vigil_monitor *m, uint64_t now)
{
    if (d->persistence == NULL) return;
    struct vigil_queue *q = &d->persistence->queue;
    struct vigil_queue_stats s;
    vigil_queue_stats(q, &s);
    bool degraded = vigil_persistence_degraded(&d->health, &s,
                                                d->degraded.active, d->persisted_revision);
    if (degraded != d->degraded.active) {
        d->degraded.active = degraded;
        if (degraded) d->degraded.raised_ms = now;
        else d->degraded.cleared_ms = now;
        ++d->degraded.transitions;
        ++d->degraded_transitions;
        alarm_event(d, 0, -3, &d->degraded, now);
    }
    uint64_t revision = m->counters.alarm_transitions + d->degraded_transitions + 1U;
    if (revision == d->persisted_revision) return;
    struct vigil_record alarms[VIGIL_ALARM_CAPACITY];
    size_t n = 0;
    for (size_t p = 0; p < m->config.pod_count; ++p) {
        const struct vigil_pod *pod = &m->pods[p];
        unsigned id = m->config.pods[p].id;
        copy_alarm(&alarms[n++], id, -1, &pod->communication, now);
        copy_alarm(&alarms[n++], id, -1, &pod->state_alarm, now);
        copy_alarm(&alarms[n++], id, -1, &pod->heartbeat.sequence, now);
        copy_alarm(&alarms[n++], id, -2, &pod->readings.sequence, now);
        for (size_t c = 0; c < VIGIL_CHANNELS; ++c) {
            if (!m->config.pods[p].source[c].enabled) continue;
            const struct vigil_source *source = &pod->source[c];
            copy_alarm(&alarms[n++], id, (int)c, &source->communication, now);
            copy_alarm(&alarms[n++], id, (int)c, &source->invalid, now);
            copy_alarm(&alarms[n++], id, (int)c, &source->flat, now);
            copy_alarm(&alarms[n++], id, (int)c, &source->high, now);
        }
    }
    copy_alarm(&alarms[n++], 0, -3, &d->degraded, now);
    vigil_queue_publish(q, alarms, n, revision);
    d->persisted_revision = revision;
}

static void persist_frame(struct diagnostics *d, const struct vigil_monitor *m,
                          const struct can_frame *frame, bool accepted,
                          enum vigil_reject reason, uint64_t now)
{
    if (d->persistence == NULL) return;
    struct vigil_message msg;
    bool decoded = vigil_decode(frame->can_id, false, frame->data, frame->len, &msg) == VIGIL_OK;
    struct vigil_record r = {.mono_ms = now, .utc_ms = vigil_utc_ms(), .channel = -1};
    const char *kind = "rejected";
    if (accepted && decoded) kind = msg.kind == VIGIL_READING ? "reading" : "heartbeat";
    (void)snprintf(r.kind, sizeof(r.kind), "%s", kind);
    r.reading = accepted && decoded && msg.kind == VIGIL_READING;
    if (decoded) { r.pod = msg.pod_id; r.channel = msg.kind == VIGIL_READING ? msg.channel : -1; }
    bool stale = true;
    if (decoded) {
        for (size_t p = 0; p < m->config.pod_count; ++p)
            if (m->config.pods[p].id == msg.pod_id)
                stale = msg.kind == VIGIL_READING ? m->pods[p].source[msg.channel].stale :
                                                  m->pods[p].heartbeat_stale;
    }
    (void)snprintf(r.detail, sizeof(r.detail),
        "{\"raw_id\":%" PRIu32 ",\"decoded\":%s,\"accepted\":%s,\"valid\":%s,\"stale\":%s,"
        "\"reason\":\"%s\",\"value\":%d,\"counter\":%u,\"status\":%u,"
        "\"pod_state\":%u,\"uptime_s\":%" PRIu32 "}",
        frame->can_id, decoded ? "true" : "false", accepted ? "true" : "false", accepted ? "true" : "false",
        stale ? "true" : "false", accepted ? "accepted" : vigil_reject_name(reason),
        decoded ? (int)msg.value : 0, decoded ? (unsigned)msg.counter : 0,
        decoded ? (unsigned)msg.status : 0, decoded ? (unsigned)msg.pod_state : 0,
        decoded ? msg.uptime_s : 0);
    vigil_queue_push(&d->persistence->queue, r);
}

static int can_open(const char *interface)
{
    unsigned index = if_nametoindex(interface);
    if (index == 0U || index > INT_MAX) { errno = ENODEV; return -1; }
    int fd = socket(PF_CAN, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, CAN_RAW);
    if (fd < 0) return -1;
    int enabled = 1;
    struct sockaddr_can address = {.can_family = AF_CAN, .can_ifindex = (int)index};
    if (setsockopt(fd, SOL_SOCKET, SO_RXQ_OVFL, &enabled, sizeof(enabled)) != 0 ||
        bind(fd, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        int saved = errno;
        (void)close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

static bool receive_batch(int fd, struct vigil_monitor *m, struct diagnostics *d)
{
    /* Bound work per poll turn so continuous CAN input cannot starve signals/timer. */
    for (unsigned n = 0; n < 64U; ++n) {
        struct can_frame frame;
        union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(sizeof(uint32_t))]; } control;
        struct iovec iov = {.iov_base = &frame, .iov_len = sizeof(frame)};
        struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1,
                             .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes)};
        ssize_t received = recvmsg(fd, &msg, 0);
        /* Linux defines EWOULDBLOCK as EAGAIN. */
        if (received < 0 && errno == EAGAIN) return true;
        if (received < 0 && errno == EINTR) continue;
        if (received < 0) return false;
        if (received != CAN_MTU || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) {
            errno = EPROTO;
            return false;
        }
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c != NULL; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_RXQ_OVFL &&
                c->cmsg_len >= CMSG_LEN(sizeof(uint32_t))) {
                uint32_t drops;
                memcpy(&drops, CMSG_DATA(c), sizeof(drops));
                d->rx_overflow += (uint32_t)(drops - d->last_rx_overflow);
                d->last_rx_overflow = drops;
            }
        }
        uint64_t now;
        if (!monotonic_ms(&now)) return false;
        vigil_monitor_tick(m, now);
        struct vigil_monitor_counters before = m->counters;
        bool accepted = vigil_monitor_frame(m, frame.can_id, false, frame.data, frame.len, now);
        enum vigil_reject reason = VIGIL_REJECT_ARGUMENT;
        if (!accepted) {
            for (int i = 0; i < VIGIL_REJECT_COUNT; ++i)
                if (m->counters.rejected[i] != before.rejected[i]) reason = (enum vigil_reject)i;
        }
        persist_frame(d, m, &frame, accepted, reason, now);
        persistence_update(d, m, now);
    }
    return true;
}

static void usage(void)
{
    fprintf(stderr, "usage: vigild [--interface vcan0] [--config FILE] [--queue-capacity N] [--outbox-path ABSOLUTE] [--no-persistence]\n"
                    "SIGUSR1: status snapshot; SIGINT/SIGTERM: clean shutdown\n");
}

int main(int argc, char **argv)
{
    const char *interface = "vcan0", *path = NULL, *outbox_path = NULL;
    bool persistence_enabled = true, queue_seen = false;
    size_t capacity = 1024;
    bool interface_seen = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 && argc == 2) { usage(); return 0; }
        if (strcmp(argv[i], "--no-persistence") == 0 && persistence_enabled) {
            persistence_enabled = false; continue;
        }
        if (i + 1 >= argc) { usage(); return 2; }
        if (strcmp(argv[i], "--interface") == 0 && !interface_seen) {
            interface = argv[++i]; interface_seen = true;
        } else if (strcmp(argv[i], "--outbox-path") == 0 && outbox_path == NULL) outbox_path = argv[++i];
        else if (strcmp(argv[i], "--config") == 0 && path == NULL) path = argv[++i];
        else if (strcmp(argv[i], "--queue-capacity") == 0 && !queue_seen) {
            const char *value = argv[++i];
            if (*value < '0' || *value > '9') { usage(); return 2; }
            char *end; errno = 0;
            unsigned long n = strtoul(value, &end, 10);
            if (errno != 0 || *end != '\0' || n == 0 || n > 65536UL) { usage(); return 2; }
            capacity = (size_t)n; queue_seen = true;
        } else { usage(); return 2; }
    }
    if (outbox_path != NULL && (!persistence_enabled || outbox_path[0] != '/')) { usage(); return 2; }
    struct vigil_config config;
    vigil_config_defaults(&config);
    char error[256];
    if (path != NULL && !vigil_config_load(path, &config, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error); return 2;
    }
    int can_fd = -1, timer_fd = -1, signal_fd = -1, result = 2;
    int output_flags = -1;
    sigset_t signals, old_mask;
    bool mask_saved = false, persistence_started = false, persistence_joined = false;
    struct vigil_persistence persistence;
    uint64_t watchdog_us = 0, last_watchdog = 0;
    int watchdog = sd_watchdog_enabled(0, &watchdog_us);
    if (watchdog < 0 || (watchdog > 0 && watchdog_us / 2000U < config.tick_ms)) {
        fprintf(stderr, "invalid watchdog interval for monitor tick\n"); return 2;
    }
    (void)sigemptyset(&signals);
    (void)sigaddset(&signals, SIGINT);
    (void)sigaddset(&signals, SIGTERM);
    (void)sigaddset(&signals, SIGUSR1);
    (void)sigaddset(&signals, SIGPIPE);
    if (sigprocmask(SIG_BLOCK, &signals, &old_mask) != 0) goto cleanup;
    mask_saved = true;
    signal_fd = signalfd(-1, &signals, SFD_CLOEXEC | SFD_NONBLOCK);
    if (signal_fd < 0) goto cleanup;
    can_fd = can_open(interface);
    if (can_fd < 0) goto cleanup;
    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (timer_fd < 0) goto cleanup;
    struct itimerspec timer = {0};
    timer.it_interval.tv_sec = (time_t)(config.tick_ms / 1000U);
    timer.it_interval.tv_nsec = (long)((config.tick_ms % 1000U) * 1000000U);
    timer.it_value = timer.it_interval;
    if (timerfd_settime(timer_fd, 0, &timer, NULL) != 0) goto cleanup;
    output_flags = fcntl(STDOUT_FILENO, F_GETFL);
    if (output_flags < 0 || fcntl(STDOUT_FILENO, F_SETFL, output_flags | O_NONBLOCK) != 0)
        goto cleanup;
    struct diagnostics diagnostics = {.degraded = {.kind = VIGIL_ALARM_PERSISTENCE,
                                                   .severity = VIGIL_WARNING}};
    if (persistence_enabled) {
        if (!vigil_persistence_start(&persistence, capacity, outbox_path == NULL ? VIGIL_OUTBOX_DEFAULT : outbox_path)) { errno = EINVAL; goto cleanup; }
        persistence_started = true;
        diagnostics.persistence = &persistence;
    }
    struct vigil_monitor monitor;
    uint64_t now;
    if (!monotonic_ms(&now) ||
        !vigil_monitor_init(&monitor, &config, now, alarm_event, &diagnostics)) goto cleanup;
    uint64_t last_status = now;
    persistence_update(&diagnostics, &monitor, now);
    last_watchdog = now;
    (void)sd_notify(0, "READY=1\nSTATUS=Monitoring CAN; local outbox ready; remote replay active");
    fprintf(stderr, "ready interface=%s pods=%zu pid=%ld\n", interface,
            config.pod_count, (long)getpid());
    snapshot(&diagnostics, &monitor, now);
    persistence_status(&diagnostics, now);
    bool stop = false;
    result = 0;
    while (!stop) {
        struct pollfd fds[] = {{.fd = signal_fd, .events = POLLIN},
                              {.fd = timer_fd, .events = POLLIN},
                              {.fd = can_fd, .events = POLLIN}};
        int ready = poll(fds, 3, -1);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) { result = 2; break; }
        bool status_requested = false;
        for (size_t i = 0; i < 3; ++i) {
            if ((fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                errno = EIO;
                result = 2;
            }
        }
        if (result != 0) break;
        if ((fds[0].revents & POLLIN) != 0) {
            struct signalfd_siginfo info;
            ssize_t bytes = read(signal_fd, &info, sizeof(info));
            if (bytes == (ssize_t)sizeof(info)) {
                if (info.ssi_signo == SIGINT || info.ssi_signo == SIGTERM) stop = true;
                if (info.ssi_signo == SIGUSR1) status_requested = true;
                /* SIGPIPE is consumed; failed output is counted, never fatal. */
            } else if (bytes >= 0 || (errno != EINTR && errno != EAGAIN)) {
                if (bytes >= 0) errno = EPROTO;
                result = 2; break;
            }
        }
        if (!monotonic_ms(&now)) { result = 2; break; }
        vigil_monitor_tick(&monitor, now);
        if (stop) break;
        if ((fds[1].revents & POLLIN) != 0) {
            uint64_t expirations;
            ssize_t bytes = read(timer_fd, &expirations, sizeof(expirations));
            if (bytes == (ssize_t)sizeof(expirations)) diagnostics.missed_ticks += expirations - 1U;
            else if (bytes >= 0 || (errno != EINTR && errno != EAGAIN)) {
                if (bytes >= 0) errno = EPROTO;
                result = 2; break;
            }
        }
        if ((fds[2].revents & POLLIN) != 0 && !receive_batch(can_fd, &monitor, &diagnostics)) {
            result = 2; break;
        }
        if (!monotonic_ms(&now)) { result = 2; break; }
        vigil_monitor_tick(&monitor, now);
        persistence_update(&diagnostics, &monitor, now);
        if (watchdog > 0 && now - last_watchdog >= watchdog_us / 2000U) {
            (void)sd_notify(0, "WATCHDOG=1");
            last_watchdog = now;
        }
        if (status_requested || (config.status_ms != 0 && now - last_status >= config.status_ms)) {
            snapshot(&diagnostics, &monitor, now);
            persistence_status(&diagnostics, now);
            last_status = now;
        }
    }
    /* Diagnostic writes can set errno (e.g. EAGAIN on a full pipe). Preserve
     * the actual event-loop failure for the fatal message after the snapshot. */
    const int loop_error = errno;
    (void)sd_notify(0, "STOPPING=1");
    if (monotonic_ms(&now)) persistence_update(&diagnostics, &monitor, now);
    if (persistence_started) {
        vigil_persistence_stop(&persistence);
        persistence_joined = true;
    }
    if (monotonic_ms(&now)) {
        /* Shutdown stopped monitoring before the drain; do not invent new
         * alarm transitions after the final history handoff. */
        snapshot(&diagnostics, &monitor, now);
        persistence_status(&diagnostics, now);
        emit(&diagnostics, "stopped now_ms=%" PRIu64 " result=%d\n", now, result);
    }
    errno = loop_error;
cleanup:
    if (persistence_started) {
        /* Normal path already joined; setup failure still needs a stop/join. */
        if (!persistence_joined) vigil_persistence_stop(&persistence);
        vigil_persistence_destroy(&persistence);
    }
    if (result != 0) fprintf(stderr, "vigild: setup or event loop failed: %s\n", strerror(errno));
    if (output_flags >= 0 && fcntl(STDOUT_FILENO, F_SETFL, output_flags) != 0) result = 2;
    if (can_fd >= 0 && close(can_fd) != 0) result = 2;
    if (timer_fd >= 0 && close(timer_fd) != 0) result = 2;
    if (signal_fd >= 0 && close(signal_fd) != 0) result = 2;
    /* Drain blocked signals before restoring the caller's mask (including a
     * SIGPIPE generated by a final diagnostic write to a closed reader). */
    if (mask_saved) {
        struct timespec no_wait = {0};
        while (sigtimedwait(&signals, NULL, &no_wait) >= 0) { }
        if (sigprocmask(SIG_SETMASK, &old_mask, NULL) != 0) result = 2;
    }
    return result;
}
