#include "vigil/replay.h"
#include "vigil/persistence.h"
#include "vigil/failpoint.h"
#include <errno.h>
#include <inttypes.h>
#include <libpq-fe.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static bool expired(struct vigil_queue *q, uint64_t deadline)
{
    struct vigil_queue_stats s;
    vigil_queue_lock(q);
    s = q->stats;
    uint64_t stop = q->stop_deadline_ms;
    vigil_queue_unlock(q);
    return vigil_now_ms() >= deadline || (s.stopping && vigil_now_ms() >= stop);
}
static bool socket_wait(struct vigil_queue *q, PGconn *conn, short events, uint64_t deadline)
{
    vigil_queue_assert_io();
    while (!expired(q, deadline)) {
        struct pollfd fd = {.fd=PQsocket(conn), .events=events};
        if (fd.fd<0) return false;
        int rc = poll(&fd,1,50);
        if (rc<0 && errno==EINTR) continue;
        if (rc<0 || (fd.revents&POLLNVAL)!=0) return false;
        if (rc>0) return true;
    }
    return false;
}
static void discard_notice(void *ctx, const char *message) { (void)ctx; (void)message; }
static PGconn *connect_db(struct vigil_persistence *p, char error[6])
{
    vigil_queue_assert_io();
    const char *keys[] = {"host","hostaddr","port","dbname","user","connect_timeout","options","application_name","gssencmode",NULL};
    const char *values[] = {p->host,p->network?p->host:"",p->port,p->database,p->user,"2",
        "-c statement_timeout=1000 -c lock_timeout=500 -c search_path=pg_catalog -c synchronous_commit=on -c default_transaction_isolation=read\\ committed",
        "vigild","disable",NULL};
    uint64_t deadline = vigil_now_ms()+2000U;
    if (expired(&p->queue,deadline)) return NULL;
    PGconn *conn = PQconnectStartParams(keys,values,0);
    if (conn==NULL) return NULL;
    (void)PQsetNoticeProcessor(conn,discard_notice,NULL);
    bool contacted=false;
    PostgresPollingStatusType status = PGRES_POLLING_WRITING;
    while (status!=PGRES_POLLING_OK) {
        if (PQstatus(conn)==CONNECTION_BAD || status==PGRES_POLLING_FAILED ||
            !socket_wait(&p->queue,conn,status==PGRES_POLLING_READING?POLLIN:POLLOUT,deadline)) {
            /* Startup ErrorResponse SQLSTATE is unavailable through libpq's
             * connection API. Treat rejected/unknown startup conservatively. */
            memcpy(error,contacted?"08004":"08001",6);
            PQfinish(conn); return NULL;
        }
        status = PQconnectPoll(conn);
        contacted=contacted || status==PGRES_POLLING_READING;
    }
    if (PQsetnonblocking(conn,1)!=0) { PQfinish(conn); return NULL; }
    return conn;
}
/* Preserve the original async deadline/poll machinery; additionally drain to
 * ReadyForQuery and check idle state before any success publication. */
static bool query(struct vigil_queue *q, PGconn *conn, const struct vigil_replay_batch *b, char error[6])
{
    vigil_queue_assert_io();
    uint64_t deadline = vigil_now_ms()+2000U;
    memcpy(error,"08006",6);
    const char *values[] = {b==NULL?NULL:b->payload};
    int lengths[] = {b==NULL?0:(int)b->bytes}, formats[] = {1};
    Oid types[] = {17}; /* bytea, binary parameter = exact persisted bytes */
    const char *sql = b==NULL ?
        "SELECT (current_setting('fsync')='on' AND current_setting('full_page_writes')='on' "
        "AND current_setting('synchronous_commit')='on' AND EXISTS(SELECT 1 FROM vigil.schema_version WHERE version=2) "
        "AND to_regprocedure('vigil.replay_batch(bytea)') IS NOT NULL)::text" :
        "SELECT * FROM vigil.replay_batch($1)";
    if (b!=NULL) vigil_test_hook("before_remote_send");
    if (expired(q,deadline)) return false;
    if (PQsendQueryParams(conn,sql,b==NULL?0:1,types,values,lengths,formats,0)==0) return false;
    if (b!=NULL) vigil_test_hook("after_remote_send");
    int flush;
    while ((flush=PQflush(conn))==1) {
        if (!socket_wait(q,conn,POLLIN|POLLOUT,deadline) || PQconsumeInput(conn)==0) return false;
    }
    if (flush<0) return false;
    bool ok = true;
    unsigned results = 0;
    for (;;) {
        while (PQisBusy(conn)!=0) {
            if (!socket_wait(q,conn,POLLIN,deadline) || PQconsumeInput(conn)==0) return false;
        }
        PGresult *r = PQgetResult(conn);
        if (r==NULL) break;
        ++results;
        if (PQresultStatus(r)!=PGRES_TUPLES_OK) {
            ok=false;
            const char *code = PQresultErrorField(r,PG_DIAG_SQLSTATE);
            if (code!=NULL && strlen(code)==5) memcpy(error,code,6);
        } else if (PQntuples(r)!=1 || PQnfields(r)!=(b==NULL?1:4)) {
            ok=false; memcpy(error,"VG001",6);
        } else if (b==NULL) {
            if (strcmp(PQgetvalue(r,0,0),"true")!=0) { ok=false; memcpy(error,"55000",6); }
        } else {
            char bid[24], count[24];
            (void)snprintf(bid,sizeof(bid),"%" PRIu64,b->id.batch_id);
            (void)snprintf(count,sizeof(count),"%" PRIu64,b->id.records);
            if (strcmp(PQgetvalue(r,0,0),b->run_id)!=0 || strcmp(PQgetvalue(r,0,1),bid)!=0 ||
                strcmp(PQgetvalue(r,0,2),count)!=0 ||
                (strcmp(PQgetvalue(r,0,3),"inserted")!=0 && strcmp(PQgetvalue(r,0,3),"equal")!=0)) {
                ok=false; memcpy(error,"VG001",6);
            }
        }
        PQclear(r);
        if (expired(q,deadline)) return false;
    }
    return ok && results==1U && PQstatus(conn)==CONNECTION_OK && PQtransactionStatus(conn)==PQTRANS_IDLE;
}
static void pause_worker(struct vigil_queue *q, uint64_t until)
{
    vigil_queue_lock(q);
    uint64_t end = q->stats.stopping && q->stop_deadline_ms<until ? q->stop_deadline_ms : until;
    vigil_queue_unlock(q);
    /* Mailbox scheduling polls at <=10 ms. Producer condition signals must
     * neither shorten remote backoff nor compete with storage's queue waiter. */
    struct timespec ts={.tv_sec=(time_t)(end/1000U),.tv_nsec=(long)((end%1000U)*1000000U)};
    int rc;
    do { rc=clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&ts,NULL); } while (rc==EINTR);
    if (rc!=0) abort();
}
static void increment(struct vigil_queue_stats *s, uint64_t *n)
{
    if (*n<(uint64_t)INT64_MAX-1U) ++*n; else s->count_exact=false;
}
void *vigil_replay_worker(void *context)
{
    struct vigil_persistence *p=context;
    struct vigil_queue *q=&p->queue;
    struct vigil_replay *slot=p->replay;
    /* Allocated by coordinator before READY, no runtime backlog allocation. */
    struct vigil_replay_batch b;
    PGconn *conn=NULL;
    uint64_t next=0, probe=0;
    unsigned delay=1;
    while (!expired(q,UINT64_MAX)) {
        vigil_queue_lock(q);
        bool initialized=q->alarm_count!=0;
        vigil_queue_unlock(q);
        if (!initialized) { pause_worker(q,vigil_now_ms()+10U); continue; }
        vigil_lock(&slot->mutex);
        bool stop=slot->stop, work=slot->state==VIGIL_SLOT_READY, poison=slot->poison;
        vigil_unlock(&slot->mutex);
        if (stop) break;
        uint64_t now=vigil_now_ms();
        if (poison || now<next || (!work && conn!=NULL && now<probe)) {
            pause_worker(q,now+10U); continue;
        }
        char error[6]="08001";
        bool connecting=conn==NULL;
        if (connecting) {
            conn=connect_db(p,error);
            if (conn!=NULL) {
                vigil_queue_lock(q);
                if (q->stats.connections!=0) increment(&q->stats,&q->stats.reconnects);
                increment(&q->stats,&q->stats.connections);
                vigil_queue_unlock(q);
            }
        }
        bool ok=conn!=NULL;
        bool submitted=false;
        if (ok && (connecting || !work)) ok=query(q,conn,NULL,error);
        if (ok && work) {
            vigil_lock(&slot->mutex);
            slot->state=VIGIL_SLOT_SENDING;
            vigil_unlock(&slot->mutex);
            b=slot->batch; /* SENDING grants PG exclusive read ownership. */
            vigil_queue_lock(q);
            increment(&q->stats,&q->stats.replay_attempts);
            q->stats.remote_state=VIGIL_REMOTE_PENDING;
            vigil_queue_unlock(q);
            submitted=true;
            ok=query(q,conn,&b,error);
            if (ok) vigil_test_hook("after_remote_confirm_before_result");
        }
        now=vigil_now_ms();
        bool fatal=!ok && (strncmp(error,"VG",2)==0 || strncmp(error,"22",2)==0 || strncmp(error,"23",2)==0);
        vigil_queue_lock(q);
        struct vigil_queue_stats *s=&q->stats;
        s->connected=ok;
        if (ok) {
            s->remote_state=VIGIL_REMOTE_CONFIRMED;
            if (s->outage_started_ms!=0) {
                s->last_outage_started_ms=s->outage_started_ms;
                s->last_outage_ended_ms=now;
                s->last_outage_ms=now-s->outage_started_ms;
                if (s->outage_total_ms<=(uint64_t)INT64_MAX-1U-s->last_outage_ms)
                    s->outage_total_ms+=s->last_outage_ms;
                else s->count_exact=false;
                s->outage_started_ms=0;
            }
        } else {
            increment(s,connecting && !submitted?&s->connection_failures:&s->query_failures);
            memcpy(s->last_error,error,6);
            s->remote_state=fatal?VIGIL_REMOTE_POISON:submitted?VIGIL_REMOTE_UNKNOWN:VIGIL_REMOTE_DISCONNECTED;
            if (s->outage_started_ms==0) { s->outage_started_ms=now; increment(s,&s->outages); }
        }
        vigil_queue_unlock(q);
        if (work && (submitted || !ok)) {
            vigil_lock(&slot->mutex);
            slot->success=ok;
            slot->poison=fatal;
            slot->state=VIGIL_SLOT_RESULT;
            vigil_unlock(&slot->mutex);
            vigil_test_hook("after_remote_result");
        }
        if (!ok) {
            if (conn!=NULL) PQfinish(conn);
            conn=NULL;
            bool transient=strncmp(error,"08",2)==0 || strncmp(error,"40",2)==0 ||
                strcmp(error,"55P03")==0 || strcmp(error,"57014")==0 || strncmp(error,"53",2)==0 || strncmp(error,"57P",3)==0;
            /* Failed startup can hide authentication/config errors; use 30 s
             * for connection rejection. Established transport errors back off. */
            unsigned seconds=!transient || strcmp(error,"08004")==0 ? 30U:delay;
            next=now+(uint64_t)seconds*1000U;
            if (delay<30U) delay=delay>=16U?30U:delay*2U;
        } else { next=0; delay=1; probe=now+1000U; }
    }
    if (conn!=NULL) PQfinish(conn);
    return NULL;
}
const char *vigil_remote_name(unsigned state)
{
    static const char *const names[]={"disconnected","pending","unknown","confirmed","poison"};
    return state<sizeof(names)/sizeof(names[0])?names[state]:"disconnected";
}
