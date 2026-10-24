/* Production outbox API, single-owner microbenchmark. Link wrapping observes
 * BEGIN..COMMIT without changing production code or SQLite settings. */
#include "vigil/outbox.h"
#include <sqlite3.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static uint64_t transaction_start, transaction_ns;
static uint64_t ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC,&t)!=0 || t.tv_sec<0) abort();
    return (uint64_t)t.tv_sec*1000000000U+(uint64_t)t.tv_nsec;
}
int __real_sqlite3_exec(sqlite3 *, const char *, int (*)(void *,int,char **,char **), void *, char **);
int __wrap_sqlite3_exec(sqlite3 *, const char *, int (*)(void *,int,char **,char **), void *, char **);
int __wrap_sqlite3_exec(sqlite3 *db,const char *sql,int (*cb)(void *,int,char **,char **),void *ctx,char **error)
{
    if (strcmp(sql,"BEGIN IMMEDIATE")==0) transaction_start=ns();
    int rc=__real_sqlite3_exec(db,sql,cb,ctx,error);
    if (strcmp(sql,"COMMIT")==0 && rc==SQLITE_OK) transaction_ns=ns()-transaction_start;
    return rc;
}
static void ok(enum vigil_store_result r) { if (r!=VIGIL_STORE_OK) abort(); }
static off_t size(const char *path,const char *suffix)
{
    char name[4096]; struct stat st;
    int n=snprintf(name,sizeof(name),"%s%s",path,suffix);
    if (n<0 || (size_t)n>=sizeof(name) || stat(name,&st)!=0) abort();
    return st.st_size;
}
int main(int argc,char **argv)
{
    if (argc!=3 || (strcmp(argv[2],"1")!=0 && strcmp(argv[2],"32")!=0)) return 2;
    size_t count=(size_t)strtoul(argv[2],NULL,10);
    struct vigil_outbox *o=NULL;
    ok(vigil_outbox_open(&o,argv[1],true)); ok(vigil_outbox_begin_run(o));
    struct vigil_queue q;
    if (!vigil_queue_init(&q,1024)) abort();
    struct vigil_record alarm={.kind="alarm",.detail="{\"active\":false}",.channel=-3};
    vigil_queue_publish(&q,&alarm,1,1);
    struct vigil_record record={.kind="reading",.detail="{\"value\":2200}",.reading=true,.pod=1,.channel=0};
    struct vigil_capture c;
    struct vigil_local_commit committed;
    printf("{\"batch_records\":%zu,\"warmup_batches\":16,\"samples\":[",count);
    for (unsigned i=0;i<216U;++i) {
        uint64_t enqueue=ns();
        for (size_t j=0;j<count;++j) {
            record.mono_ms=vigil_now_ms(); record.utc_ms=vigil_utc_ms();
            if (vigil_queue_push(&q,record)!=VIGIL_QUEUED) abort();
        }
        vigil_queue_take_local(&q,&c);
        if (c.count!=count) abort();
        uint64_t start=ns(); ok(vigil_outbox_append(o,&c,&committed)); uint64_t end=ns();
        if (committed.records!=count) abort();
        if (i>=16U)
            printf("%s{\"append_ns\":%" PRIu64 ",\"queue_to_commit_ns\":%" PRIu64 ",\"transaction_ns\":%" PRIu64
                   ",\"db_bytes\":%jd,\"wal_bytes\":%jd}",i==16U?"":",",end-start,end-enqueue,transaction_ns,
                   (intmax_t)size(argv[1],""),(intmax_t)size(argv[1],"-wal"));
        /* Same held-to-local publication boundary; no remote acknowledgement. */
        vigil_queue_lock(&q); q.stats.in_flight=0; q.stats.in_flight_readings=0; vigil_queue_unlock(&q);
    }
    uint64_t start=ns(); ok(vigil_outbox_checkpoint(o,true)); uint64_t checkpoint=ns()-start;
    struct vigil_outbox_usage usage; vigil_outbox_usage(o,&usage);
    if (usage.records!=216U*count || usage.retired!=0) abort();
    printf("],\"checkpoint_ns\":%" PRIu64 ",\"checkpoint_db_bytes\":%jd,\"checkpoint_wal_bytes\":%jd,\"durable_records\":%" PRIu64 "}\n",
           checkpoint,(intmax_t)size(argv[1],""),(intmax_t)size(argv[1],"-wal"),usage.records);
    ok(vigil_outbox_close(o)); vigil_queue_destroy(&q);
    return 0;
}
