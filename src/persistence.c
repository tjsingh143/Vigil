#include "vigil/persistence.h"
#include "vigil/outbox.h"
#include "vigil/replay.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void pause_until(struct vigil_queue *q, uint64_t until, bool retry)
{
    vigil_queue_lock(q);
    for (;;) {
        uint64_t limit = until;
        if (q->stats.stopping && q->stop_deadline_ms < limit) limit = q->stop_deadline_ms;
        if (vigil_now_ms() >= limit) break;
        if (!retry && (q->stats.stopping ||
                       (q->alarm_count != 0 &&
                        (q->stats.reading_depth + q->stats.event_depth >= VIGIL_BATCH_SIZE ||
                         q->alarm_revision != q->stats.local_revision))))
            break;
        struct timespec ts = {.tv_sec = (time_t)(limit / 1000U),
                              .tv_nsec = (long)((limit % 1000U) * 1000000U)};
        int rc = pthread_cond_timedwait(&q->changed, &q->mutex, &ts);
        if (rc == ETIMEDOUT) break;
        if (rc != 0) abort();
    }
    vigil_queue_unlock(q);
}
static void publish(struct vigil_persistence *p, struct vigil_outbox *o, enum vigil_store_result r,
                    const struct vigil_capture *c)
{
    struct vigil_outbox_usage u;
    vigil_outbox_usage(o, &u);
    vigil_queue_lock(&p->queue);
    struct vigil_queue_stats *s = &p->queue.stats;
    s->local_state = (unsigned)r;
    s->sqlite_error = vigil_outbox_error(o);
    if (r == VIGIL_STORE_OK) {
        s->durable_records = u.records;
        s->durable_batches = u.batches;
        s->durable_bytes = u.bytes;
        s->spool_admitted = u.admitted;
        s->spool_retired = u.retired;
        if (c != NULL) {
            s->locally_committed += s->in_flight;
            s->locally_committed_readings += s->in_flight_readings;
            s->in_flight = 0;
            s->in_flight_readings = 0;
            s->local_revision = c->revision;
            s->local_reading_drops = c->stats.readings_dropped;
            s->local_event_drops = c->stats.events_dropped;
        }
    } else if (s->local_failures < INT64_MAX)
        ++s->local_failures;
    else
        s->count_exact = false;
    vigil_queue_unlock(&p->queue);
}
static void retired(struct vigil_persistence *p, struct vigil_outbox *o, const struct vigil_replay_batch *b)
{
    struct vigil_outbox_usage u;
    vigil_outbox_usage(o,&u);
    vigil_queue_lock(&p->queue);
    struct vigil_queue_stats *s=&p->queue.stats;
    s->local_state=VIGIL_STORE_OK;
    s->sqlite_error=0;
    s->durable_records=u.records; s->durable_batches=u.batches; s->durable_bytes=u.bytes;
    s->spool_admitted=u.admitted; s->spool_retired=u.retired;
    if (strcmp(b->run_id,p->run_id)==0) {
        s->records_written+=b->id.records;
        s->readings_written+=b->id.readings;
        s->acknowledged_revision=b->revision;
        s->acknowledged_reading_drops=b->reading_drops;
        s->acknowledged_event_drops=b->event_drops;
    }
    ++s->batches_written;
    if (b->gap) ++s->gap_markers;
    if (b->snapshot) ++s->snapshots_written;
    s->oldest_pending_order=u.batches==0?0:b->id.order_id+1U;
    vigil_queue_unlock(&p->queue);
}
static void *writer(void *context)
{
    struct vigil_persistence *p=context;
    struct vigil_queue *q=&p->queue;
    struct vigil_replay *slot=p->replay;
    struct vigil_outbox *o=NULL;
    struct vigil_capture *c=calloc(1,sizeof(*c));
    enum vigil_store_result r=c==NULL?VIGIL_STORE_NOMEM:vigil_outbox_open(&o,p->outbox_path,false);
    if (r==VIGIL_STORE_OK) r=vigil_outbox_begin_run(o);
    if (r==VIGIL_STORE_OK) {
        memcpy(p->run_id,vigil_outbox_run_id(o),sizeof(p->run_id));
        publish(p,o,r,NULL);
    }
    vigil_queue_lock(q);
    p->startup_done=true; p->startup_ok=r==VIGIL_STORE_OK;
    q->stats.local_ready=p->startup_ok; q->stats.local_state=(unsigned)r;
    (void)pthread_cond_broadcast(&q->changed);
    vigil_queue_unlock(q);
    bool held=false, latched=false, recovery=false, failed=false;
    uint64_t last_checkpoint=vigil_now_ms(), local_retry=0, retire_retry=0;
    while (c!=NULL && p->startup_ok) {
        vigil_queue_lock(q);
        bool stop=q->stats.stopping, ready=q->alarm_count!=0;
        uint64_t deadline=q->stop_deadline_ms;
        vigil_queue_unlock(q);
        uint64_t now=vigil_now_ms();
        if (stop && now>=deadline) break;
        vigil_lock(&slot->mutex);
        enum vigil_slot_state state=slot->state;
        bool success=slot->success, poison=slot->poison;
        vigil_unlock(&slot->mutex);
        if (!latched && state==VIGIL_SLOT_RESULT && now>=retire_retry) {
            if (success) {
                r=vigil_outbox_retire(o,&slot->batch);
                if (r==VIGIL_STORE_OK) {
                    retired(p,o,&slot->batch);
                    recovery=recovery||failed;
                    failed=false;
                } else {
                    publish(p,o,r,NULL);
                    retire_retry=now+1000U;
                    latched=r==VIGIL_STORE_CORRUPT || r==VIGIL_STORE_INVALID || r==VIGIL_STORE_EXHAUSTED;
                }
            } else { failed=true; r=VIGIL_STORE_OK; }
            if (r==VIGIL_STORE_OK) {
                vigil_lock(&slot->mutex);
                slot->state=VIGIL_SLOT_EMPTY;
                vigil_unlock(&slot->mutex);
                state=VIGIL_SLOT_EMPTY;
            }
        }
        /* A retirement error may have committed its prefix. Reconcile and
         * publish that result before another mutation exposes newer usage;
         * otherwise spool totals can outrun current-run acknowledgements. */
        bool retire_blocked=state==VIGIL_SLOT_RESULT && success && now<retire_retry;
        if (!latched && !retire_blocked && ready && now>=local_retry) {
            if (!held) {
                vigil_queue_take_local(q,c);
                struct vigil_queue_stats s;
                vigil_queue_stats(q,&s);
                c->recovery=recovery;
                held=c->count!=0 || c->revision!=s.local_revision || recovery ||
                    c->stats.readings_dropped!=s.local_reading_drops ||
                    c->stats.events_dropped!=s.local_event_drops;
            }
            if (held) {
                struct vigil_local_commit commit;
                r=vigil_outbox_append(o,c,&commit);
                publish(p,o,r,r==VIGIL_STORE_OK?c:NULL);
                if (r==VIGIL_STORE_OK) {
                    held=false;
                    if (c->recovery) recovery=false;
                } else {
                    local_retry=now+1000U;
                    latched=r==VIGIL_STORE_CORRUPT || r==VIGIL_STORE_INVALID || r==VIGIL_STORE_EXHAUSTED;
                }
            }
        }
        if (!latched && !poison && state==VIGIL_SLOT_EMPTY) {
            /* PG only reads READY/SENDING; storage has exclusive ownership of
             * EMPTY and RESULT payloads. Never copy payload under a queue lock. */
            r=vigil_outbox_peek(o,&slot->batch);
            if (r==VIGIL_STORE_OK && slot->batch.bytes!=0) {
                vigil_queue_lock(q);
                q->stats.oldest_pending_order=slot->batch.id.order_id;
                vigil_queue_unlock(q);
                vigil_lock(&slot->mutex);
                slot->state=VIGIL_SLOT_READY;
                vigil_unlock(&slot->mutex);
                state=VIGIL_SLOT_READY;
            } else if (r!=VIGIL_STORE_OK && now>=local_retry) {
                publish(p,o,r,NULL);
                local_retry=now+1000U;
                latched=r==VIGIL_STORE_CORRUPT || r==VIGIL_STORE_INVALID || r==VIGIL_STORE_EXHAUSTED;
            }
        }
        if (!latched && !retire_blocked && now-last_checkpoint>=1000U) {
            r=vigil_outbox_checkpoint(o,false);
            last_checkpoint=now;
            if (r!=VIGIL_STORE_OK) {
                publish(p,o,r,NULL);
                latched=r==VIGIL_STORE_CORRUPT || r==VIGIL_STORE_INVALID || r==VIGIL_STORE_EXHAUSTED;
            }
        }
        if (stop && !held && state==VIGIL_SLOT_EMPTY) {
            struct vigil_queue_stats s;
            vigil_queue_stats(q,&s);
            if (s.reading_depth+s.event_depth==0) break;
        }
        pause_until(q,vigil_now_ms()+10U,held || latched || stop || retire_blocked || now<local_retry);
    }
    vigil_lock(&slot->mutex);
    slot->stop=true;
    vigil_unlock(&slot->mutex);
    if (o!=NULL) {
        enum vigil_store_result end=vigil_outbox_close(o);
        if (end!=VIGIL_STORE_OK) {
            vigil_queue_lock(q);
            q->stats.local_state=(unsigned)end;
            ++q->stats.local_failures;
            vigil_queue_unlock(q);
        }
    }
    free(c);
    vigil_queue_lock(q);
    q->stats.abandoned_readings=q->stats.reading_depth+q->stats.in_flight_readings;
    q->stats.abandoned_events=q->stats.event_depth+q->stats.in_flight-q->stats.in_flight_readings;
    vigil_queue_unlock(q);
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

bool vigil_persistence_start(struct vigil_persistence *p, size_t capacity, const char *outbox_path)
{
    memset(p, 0, sizeof(*p));
    /* Fixed numeric/Unix endpoint avoids DNS blocking inside PQconnectStart.
     * Service/hostaddr indirection would defeat that restriction. */
    if (getenv("PGSERVICE") != NULL || getenv("PGHOSTADDR") != NULL ||
        !copy_env(p->host, sizeof(p->host), "VIGIL_DB_HOST", "/var/run/postgresql") ||
        !copy_env(p->port, sizeof(p->port), "VIGIL_DB_PORT", "5432") ||
        !copy_env(p->database, sizeof(p->database), "VIGIL_DB_NAME", "vigil") ||
        !copy_env(p->user, sizeof(p->user), "VIGIL_DB_USER", "vigil"))
        return false;
    for (size_t i = 0; p->port[i] != '\0'; ++i)
        if (p->port[i] < '0' || p->port[i] > '9') return false;
    unsigned long port = strtoul(p->port, NULL, 10);
    if (port == 0 || port > 65535UL) return false;
    if (strchr(p->host, ',') != NULL) return false; /* disallow libpq multi-host/DNS indirection */
    if (p->host[0] != '/') {
        unsigned char address[16];
        if (inet_pton(AF_INET, p->host, address) != 1 && inet_pton(AF_INET6, p->host, address) != 1)
            return false;
        p->network = true;
    }
    if (outbox_path == NULL || outbox_path[0] != '/' ||
        strlen(outbox_path) >= sizeof(p->outbox_path))
        return false;
    memcpy(p->outbox_path, outbox_path, strlen(outbox_path) + 1U);
    if (!vigil_queue_init(&p->queue, capacity)) return false;
    p->replay=calloc(1,sizeof(*p->replay));
    if (p->replay==NULL || pthread_mutex_init(&p->replay->mutex,NULL)!=0) {
        free(p->replay); vigil_queue_destroy(&p->queue); return false;
    }
    p->queue.stats.local_mode = true;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        vigil_queue_destroy(&p->queue);
        (void)pthread_mutex_destroy(&p->replay->mutex); free(p->replay);
        return false;
    }
    int rc = pthread_attr_setstacksize(&attr, 1024U * 1024U);
    if (rc == 0) rc = pthread_create(&p->thread, &attr, writer, p);
    (void)pthread_attr_destroy(&attr);
    if (rc != 0) {
        vigil_queue_destroy(&p->queue);
        (void)pthread_mutex_destroy(&p->replay->mutex); free(p->replay);
        return false;
    }
    vigil_queue_lock(&p->queue);
    while (!p->startup_done)
        (void)pthread_cond_wait(&p->queue.changed, &p->queue.mutex);
    bool ok = p->startup_ok;
    vigil_queue_unlock(&p->queue);
    if (ok) {
        int attr_rc=pthread_attr_init(&attr);
        if (attr_rc==0) {
            attr_rc=pthread_attr_setstacksize(&attr,1024U*1024U);
            if (attr_rc==0) attr_rc=pthread_create(&p->replay_thread,&attr,vigil_replay_worker,p);
            (void)pthread_attr_destroy(&attr);
        }
        if (attr_rc!=0) {
            vigil_queue_lock(&p->queue);
            p->queue.stats.stopping=true;
            p->queue.stop_deadline_ms=vigil_now_ms();
            (void)pthread_cond_broadcast(&p->queue.changed);
            vigil_queue_unlock(&p->queue);
            ok=false;
        }
    }
    if (!ok) {
        (void)pthread_join(p->thread,NULL);
        (void)pthread_mutex_destroy(&p->replay->mutex); free(p->replay);
        vigil_queue_destroy(&p->queue);
    }
    return ok;
}
void vigil_persistence_stop(struct vigil_persistence *p)
{
    vigil_queue_lock(&p->queue);
    p->queue.stats.stopping = true;
    p->queue.stop_deadline_ms = vigil_now_ms() + 1000U;
    (void)pthread_cond_broadcast(&p->queue.changed);
    vigil_queue_unlock(&p->queue);
    (void)pthread_join(p->replay_thread,NULL);
    (void)pthread_join(p->thread,NULL);
}
void vigil_persistence_destroy(struct vigil_persistence *p)
{
    vigil_queue_destroy(&p->queue);
    (void)pthread_mutex_destroy(&p->replay->mutex);
    free(p->replay);
}
