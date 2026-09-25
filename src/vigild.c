#include "vigil/monitor.h"

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
#include <string.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

struct diagnostics {
    uint64_t dropped, snapshots, rx_overflow, missed_ticks;
    uint32_t last_rx_overflow;
};

static void emit(struct diagnostics *d, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

static void emit(struct diagnostics *d, const char *format, ...)
{
    /* One <= PIPE_BUF nonblocking write per line, no unbounded output queue. */
    char line[1024];
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

static void alarm_event(void *context, uint8_t pod, int channel,
                         const struct vigil_alarm *alarm, uint64_t now)
{
    struct diagnostics *d = context;
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
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
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
        (void)vigil_monitor_frame(m, frame.can_id, false, frame.data, frame.len, now);
    }
    return true;
}

static void usage(void)
{
    fprintf(stderr, "usage: vigild [--interface vcan0] [--config FILE]\n"
                    "SIGUSR1: status snapshot; SIGINT/SIGTERM: clean shutdown\n");
}

int main(int argc, char **argv)
{
    const char *interface = "vcan0", *path = NULL;
    bool interface_seen = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 && argc == 2) { usage(); return 0; }
        if (i + 1 >= argc) { usage(); return 2; }
        if (strcmp(argv[i], "--interface") == 0 && !interface_seen) {
            interface = argv[++i]; interface_seen = true;
        } else if (strcmp(argv[i], "--config") == 0 && path == NULL) path = argv[++i];
        else { usage(); return 2; }
    }
    struct vigil_config config;
    vigil_config_defaults(&config);
    char error[256];
    if (path != NULL && !vigil_config_load(path, &config, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error); return 2;
    }
    int can_fd = -1, timer_fd = -1, signal_fd = -1, result = 2;
    int output_flags = -1;
    sigset_t signals, old_mask;
    bool mask_saved = false;
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
    struct diagnostics diagnostics = {0};
    struct vigil_monitor monitor;
    uint64_t now;
    if (!monotonic_ms(&now) ||
        !vigil_monitor_init(&monitor, &config, now, alarm_event, &diagnostics)) goto cleanup;
    uint64_t last_status = now;
    fprintf(stderr, "ready interface=%s pods=%zu pid=%ld\n", interface,
            config.pod_count, (long)getpid());
    snapshot(&diagnostics, &monitor, now);
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
        if (status_requested || (config.status_ms != 0 && now - last_status >= config.status_ms)) {
            snapshot(&diagnostics, &monitor, now);
            last_status = now;
        }
    }
    /* Diagnostic writes can set errno (e.g. EAGAIN on a full pipe). Preserve
     * the actual event-loop failure for the fatal message after the snapshot. */
    const int loop_error = errno;
    if (monotonic_ms(&now)) {
        vigil_monitor_tick(&monitor, now);
        snapshot(&diagnostics, &monitor, now);
        emit(&diagnostics, "stopped now_ms=%" PRIu64 " result=%d\n", now, result);
    }
    errno = loop_error;
cleanup:
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
