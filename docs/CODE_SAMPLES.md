# Vigil — Code samples

Six excerpts from the published source. The verification guide separates hosted test results, the fresh portable checks, and the physical and service integration work that was not repeated during this documentation review.

Reviewed October 11, 2026. Every code block below is a contiguous, verbatim excerpt from source at commit `c66b28d`. Links pin the exact file and line range. Separate ranges have separate blocks; no code is rewritten or elided inside a block.

The older Drive source copy remains a historical archive. Current source is in GitHub; private repositories require existing access.

## 1. Validation order and signed conversion

Frame format, identifier, length and CRC are checked before payload fields. The signed value uses explicit subtraction rather than an implementation-defined unsigned-to-signed conversion; the output is assigned only after successful decoding.

[src/protocol.c, lines 20–81](https://github.com/tjsingh143/Vigil/blob/c66b28d705e44e957279dc3c168979d11b16f7a3/src/protocol.c#L20-L81)

```c
enum vigil_result vigil_decode(uint32_t raw_id, bool is_fd,
                               const uint8_t *data, size_t length,
                               struct vigil_message *out)
{
    struct vigil_message msg = {0};
    size_t expected;
    uint8_t crc;
    if (data == NULL || out == NULL) {
        return VIGIL_ERR_ARGUMENT;
    }
    if (is_fd || (raw_id & UINT32_C(0xe0000000)) != 0U) {
        return VIGIL_ERR_FORMAT;
    }
    if (raw_id >= 0x101U && raw_id <= 0x17fU) {
        msg.kind = VIGIL_READING;
        msg.pod_id = (uint8_t)(raw_id - 0x100U);
        expected = 6;
    } else if (raw_id >= 0x701U && raw_id <= 0x77fU) {
        msg.kind = VIGIL_HEARTBEAT;
        msg.pod_id = (uint8_t)(raw_id - 0x700U);
        expected = 7;
    } else {
        return VIGIL_ERR_ID;
    }
    if (length != expected) {
        return VIGIL_ERR_LENGTH;
    }
    (void)vigil_crc8(data, length - 1U, &crc);
    if (crc != data[length - 1U]) {
        return VIGIL_ERR_CRC;
    }
    if (msg.kind == VIGIL_READING) {
        uint32_t raw_value = (uint32_t)data[1] | ((uint32_t)data[2] << 8U);
        /* Explicit subtraction avoids implementation-defined unsigned->signed
         * conversion for values outside int16_t's positive range. */
        int32_t signed_value = raw_value <= INT16_MAX ? (int32_t)raw_value :
                               (int32_t)raw_value - INT32_C(65536);
        if (data[0] > VIGIL_DOOR) {
            return VIGIL_ERR_CHANNEL;
        }
        if (data[3] > VIGIL_STATUS_FAULT) {
            return VIGIL_ERR_STATUS;
        }
        if (data[0] == VIGIL_DOOR && signed_value != 0 && signed_value != 1) {
            return VIGIL_ERR_VALUE;
        }
        msg.channel = data[0];
        msg.value = (int16_t)signed_value;
        msg.status = data[3];
        msg.counter = data[4];
    } else {
        if (data[0] > VIGIL_POD_FAULT) {
            return VIGIL_ERR_STATE;
        }
        msg.pod_state = data[0];
        msg.uptime_s = (uint32_t)data[1] | ((uint32_t)data[2] << 8U) |
                       ((uint32_t)data[3] << 16U) | ((uint32_t)data[4] << 24U);
        msg.counter = data[5];
    }
    *out = msg;
    return VIGIL_OK;
}
```

## 2. Advancing an excluded channel’s sequence state

The shared reading-stream counter advances before an unconfigured channel is rejected. Monitoring only one channel therefore does not turn traffic from its sibling into inferred loss.

[src/monitor.c, lines 187–201](https://github.com/tjsingh143/Vigil/blob/c66b28d705e44e957279dc3c168979d11b16f7a3/src/monitor.c#L187-L201)

```c
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
```

## 3. Retaining an in-flight batch

A new batch is taken only when no batch is in flight. Queue overflow accounting and acknowledgement are separate paths; this boundary prevents replacing a payload whose commit response may be ambiguous.

[src/queue.c, lines 74–99](https://github.com/tjsingh143/Vigil/blob/c66b28d705e44e957279dc3c168979d11b16f7a3/src/queue.c#L74-L99)

```c
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
```

## 4. Bounding socket waits

The nonblocking libpq path polls in short intervals and checks both shutdown and the operation deadline. The server’s timeout alone cannot bound a lost connection.

[src/persistence.c, lines 25–45](https://github.com/tjsingh143/Vigil/blob/c66b28d705e44e957279dc3c168979d11b16f7a3/src/persistence.c#L25-L45)

```c
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
```

## 5. Retrying a lost commit response

A run and batch identity gates the atomic write. An already-recorded checkpoint returns immediately, while history rows also have conflict protection. The writer retains the original payload and identity when retrying an ambiguous commit.

[migrations/001_history.sql, lines 40–64](https://github.com/tjsingh143/Vigil/blob/c66b28d705e44e957279dc3c168979d11b16f7a3/migrations/001_history.sql#L40-L64)

```sql
CREATE OR REPLACE FUNCTION vigil.write_batch(b jsonb) RETURNS void
LANGUAGE plpgsql SET search_path = pg_catalog AS $$
DECLARE
    rid uuid := (b->>'run_id')::uuid;
    bid bigint := (b->>'batch_id')::bigint;
BEGIN
    IF EXISTS (SELECT 1 FROM vigil.checkpoints WHERE run_id=rid AND batch_id=bid) THEN
        RETURN;
    END IF;
    INSERT INTO vigil.runs VALUES (rid,
        to_timestamp((b->>'started_utc_ms')::double precision/1000),
        (b->>'started_ms')::bigint) ON CONFLICT DO NOTHING;
    INSERT INTO vigil.history
    SELECT rid, x.seq, to_timestamp(x.utc_ms::double precision/1000),
           x.mono_ms, x.kind, x.pod, x.channel, x.detail
    FROM jsonb_to_recordset(b->'records') AS x(seq bigint,utc_ms bigint,
        mono_ms bigint,kind text,pod smallint,channel smallint,detail jsonb)
    ON CONFLICT DO NOTHING;
    INSERT INTO vigil.checkpoints VALUES (rid,bid,
        to_timestamp((b->>'utc_ms')::double precision/1000),
        (b->>'mono_ms')::bigint,b->>'kind',
        (b->>'reading_drops')::bigint,(b->>'event_drops')::bigint,
        b->'health',NULLIF(b->'alarms','null'::jsonb));
END;
$$;
```

## 6. Testing an exact timeout boundary

Injected timestamps pin the last current instant, the stale transition, retained reading, and absence of repeated timeout events. This test needs no real sleep.

[tests/test_monitor.c, lines 144–160](https://github.com/tjsingh143/Vigil/blob/c66b28d705e44e957279dc3c168979d11b16f7a3/tests/test_monitor.c#L144-L160)

```c
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
```
