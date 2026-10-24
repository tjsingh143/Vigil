#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include "vigil/outbox.h"
#include "vigil/persistence.h"
#include <sqlite3.h>
#include <limits.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct fixture {
    char dir[80], path[128];
    struct vigil_outbox *o;
};
static struct vigil_capture capture(size_t n)
{
    struct vigil_capture c = {.count = n, .alarm_count = 1, .revision = 1, .ticket = n};
    c.alarms[0] =
        (struct vigil_record){.kind = "alarm", .detail = "{\"active\":false}", .channel = -3};
    for (size_t i = 0; i < n; ++i)
        c.records[i] = (struct vigil_record){.seq = i + 1U,
                                             .mono_ms = 100U + i,
                                             .utc_ms = 1000U + i,
                                             .reading = i % 2U == 0,
                                             .pod = 1,
                                             .channel = 0};
    for (size_t i = 0; i < n; ++i) {
        (void)snprintf(c.records[i].kind, sizeof(c.records[i].kind), "%s",
                       c.records[i].reading ? "reading" : "heartbeat");
        (void)snprintf(c.records[i].detail, sizeof(c.records[i].detail), "{\"value\":%zu}", i);
    }
    return c;
}
static int setup(void **state)
{
    struct fixture *f = calloc(1, sizeof(*f));
    assert_non_null(f);
    (void)snprintf(f->dir, sizeof(f->dir), "/tmp/vigil-outbox-unit-XXXXXX");
    assert_non_null(mkdtemp(f->dir));
    (void)snprintf(f->path, sizeof(f->path), "%s/outbox.sqlite3", f->dir);
    assert_int_equal(vigil_outbox_open(&f->o, f->path, true), VIGIL_STORE_OK);
    assert_int_equal(vigil_outbox_close(f->o), VIGIL_STORE_OK);
    f->o = NULL;
    assert_int_equal(vigil_outbox_open(&f->o, f->path, false), VIGIL_STORE_OK);
    assert_int_equal(vigil_outbox_begin_run(f->o), VIGIL_STORE_OK);
    *state = f;
    return 0;
}
static int teardown(void **state)
{
    struct fixture *f = *state;
    (void)unsetenv("VIGIL_TEST_FAULT");
    (void)unsetenv("VIGIL_TEST_RECORD_LIMIT");
    (void)unsetenv("VIGIL_TEST_BATCH_LIMIT");
    (void)unsetenv("VIGIL_TEST_BYTE_LIMIT");
    if (f->o != NULL) assert_int_equal(vigil_outbox_close(f->o), VIGIL_STORE_OK);
    const char *suffix[] = {"", "-wal", "-shm"};
    char path[140];
    for (size_t i = 0; i < 3; ++i) {
        (void)snprintf(path, sizeof(path), "%s%s", f->path, suffix[i]);
        (void)unlink(path);
    }
    (void)snprintf(path, sizeof(path), "%s/outbox.lock", f->dir);
    assert_int_equal(unlink(path), 0);
    assert_int_equal(rmdir(f->dir), 0);
    free(f);
    return 0;
}
static sqlite3 *inspect(struct fixture *f)
{
    assert_int_equal(vigil_outbox_close(f->o), VIGIL_STORE_OK);
    f->o = NULL;
    sqlite3 *db = NULL;
    assert_int_equal(sqlite3_open(f->path, &db), SQLITE_OK);
    return db;
}
static int64_t number(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *s = NULL;
    assert_int_equal(sqlite3_prepare_v2(db, sql, -1, &s, NULL), SQLITE_OK);
    assert_int_equal(sqlite3_step(s), SQLITE_ROW);
    int64_t n = sqlite3_column_int64(s, 0);
    assert_int_equal(sqlite3_finalize(s), SQLITE_OK);
    return n;
}
static void identities_order_reopen(void **state)
{
    struct fixture *f = *state;
    char run[37];
    memcpy(run, vigil_outbox_run_id(f->o), 37);
    struct vigil_capture c = capture(3);
    struct vigil_local_commit a, b;
    assert_int_equal(vigil_outbox_append(f->o, &c, &a), VIGIL_STORE_OK);
    assert_int_equal(vigil_outbox_append(f->o, &c, &b), VIGIL_STORE_OK);
    assert_int_equal(a.first_seq, 1);
    assert_int_equal(b.first_seq, 4);
    assert_int_equal(a.order_id, 1);
    assert_int_equal(b.order_id, 2);
    sqlite3 *db = inspect(f);
    assert_int_equal(number(db, "SELECT admitted_records FROM spool_meta"), 6);
    assert_int_equal(number(db, "SELECT sum(reading_count) FROM outbox_batches"), 4);
    assert_int_equal(
        number(
            db,
            "SELECT json_extract(payload,'$.records[2].seq') FROM outbox_batches WHERE order_id=2"),
        6);
    assert_int_equal(number(db, "SELECT count(*) FROM outbox_batches WHERE "
                                "json_extract(payload,'$.origin.run_id')=run_id"),
                     2);
    assert_int_equal(
        sqlite3_exec(db, "UPDATE outbox_batches SET payload=payload", NULL, NULL, NULL) & 255,
        SQLITE_CONSTRAINT);
    assert_int_equal(sqlite3_close(db), SQLITE_OK);
    assert_int_equal(vigil_outbox_open(&f->o, f->path, false), VIGIL_STORE_OK);
    assert_int_equal(vigil_outbox_begin_run(f->o), VIGIL_STORE_OK);
    assert_string_not_equal(run, vigil_outbox_run_id(f->o));
    assert_int_equal(vigil_outbox_append(f->o, &c, &a), VIGIL_STORE_OK);
    assert_int_equal(a.first_seq, 1);
    assert_int_equal(a.order_id, 3);
    assert_int_equal(a.batch_id, 1);
}
static void atomic_rollback(void **state)
{
    struct fixture *f = *state;
    struct vigil_capture c = capture(2);
    struct vigil_local_commit a;
    assert_int_equal(setenv("VIGIL_TEST_FAULT", "rollback", 1), 0);
    assert_int_equal(vigil_outbox_append(f->o, &c, &a), VIGIL_STORE_FULL);
    struct vigil_outbox_usage u;
    vigil_outbox_usage(f->o, &u);
    assert_int_equal(u.records, 0);
    assert_int_equal(unsetenv("VIGIL_TEST_FAULT"), 0);
    c.revision = 2; /* A retry must keep the original frozen metadata too. */
    assert_int_equal(vigil_outbox_append(f->o, &c, &a), VIGIL_STORE_OK);
    assert_int_equal(a.first_seq, 1);
    assert_int_equal(a.order_id, 1);
    assert_int_equal(a.batch_id, 1);
    sqlite3 *db = inspect(f);
    assert_int_equal(number(db, "SELECT next_seq FROM origin_runs"), 3);
    assert_int_equal(number(db, "SELECT next_order_id FROM spool_meta"), 2);
    assert_int_equal(number(db, "SELECT pending_records FROM spool_meta"), 2);
    assert_int_equal(number(db, "SELECT captured_revision FROM origin_runs"), 1);
    assert_int_equal(sqlite3_close(db), SQLITE_OK);
}
static void capacity_records(void **state)
{
    struct fixture *f = *state;
    struct vigil_capture c = capture(3);
    struct vigil_local_commit a;
    assert_int_equal(setenv("VIGIL_TEST_RECORD_LIMIT", "3", 1), 0);
    assert_int_equal(vigil_outbox_append(f->o, &c, &a), VIGIL_STORE_OK);
    assert_int_equal(vigil_outbox_append(f->o, &c, &a), VIGIL_STORE_FULL);
    struct vigil_outbox_usage u;
    vigil_outbox_usage(f->o, &u);
    assert_int_equal(u.records, 3);
    sqlite3 *db = inspect(f);
    assert_int_equal(number(db, "SELECT count(*) FROM outbox_batches"), 1);
    assert_int_equal(number(db, "SELECT next_seq FROM origin_runs"), 4);
    assert_int_equal(sqlite3_close(db), SQLITE_OK);
}
static void capacity_batches(void **state)
{
    struct fixture *f = *state;
    struct vigil_capture c = capture(0);
    struct vigil_local_commit a;
    assert_int_equal(setenv("VIGIL_TEST_BATCH_LIMIT", "2", 1), 0);
    assert_int_equal(vigil_outbox_append(f->o, &c, &a), VIGIL_STORE_OK);
    ++c.revision;
    assert_int_equal(vigil_outbox_append(f->o, &c, &a), VIGIL_STORE_OK);
    ++c.revision;
    assert_int_equal(vigil_outbox_append(f->o, &c, &a), VIGIL_STORE_FULL);
    struct vigil_outbox_usage u;
    vigil_outbox_usage(f->o, &u);
    assert_int_equal(u.batches, 2);
    assert_int_equal(u.records, 0);
}
static void capacity_bytes(void **state)
{
    struct fixture *f = *state;
    struct vigil_capture c = capture(1);
    struct vigil_local_commit a;
    assert_int_equal(vigil_outbox_append(f->o, &c, &a), VIGIL_STORE_OK);
    struct vigil_outbox_usage u;
    vigil_outbox_usage(f->o, &u);
    char limit[40];
    (void)snprintf(limit, sizeof(limit), "%" PRIu64, u.bytes);
    assert_int_equal(setenv("VIGIL_TEST_BYTE_LIMIT", limit, 1), 0);
    assert_int_equal(vigil_outbox_append(f->o, &c, &a), VIGIL_STORE_FULL);
    struct vigil_outbox_usage v;
    vigil_outbox_usage(f->o, &v);
    assert_int_equal(u.bytes, v.bytes);
    assert_int_equal(v.records, 1);
}
static void failed_writes(void **state)
{
    struct fixture *f = *state;
    struct vigil_capture c = capture(2);
    struct vigil_local_commit a;
    const char *faults[] = {"write_full", "write_readonly", "lock_busy", "write_io",
                            "sync_io",    "directory_sync", "read_io"};
    const enum vigil_store_result expected[] = {VIGIL_STORE_FULL,       VIGIL_STORE_READONLY,
                                                VIGIL_STORE_BUSY,       VIGIL_STORE_IO_UNKNOWN,
                                                VIGIL_STORE_IO_UNKNOWN, VIGIL_STORE_IO_UNKNOWN,
                                                VIGIL_STORE_IO_UNKNOWN};
    for (size_t i = 0; i < 7; ++i) {
        assert_int_equal(setenv("VIGIL_TEST_FAULT", faults[i], 1), 0);
        enum vigil_store_result got = vigil_outbox_append(f->o, &c, &a);
        print_message("fault=%s result=%s sqlite=%d\n", faults[i], vigil_store_name(got),
                      vigil_outbox_error(f->o));
        assert_int_equal(got, expected[i]);
        struct vigil_outbox_usage u;
        vigil_outbox_usage(f->o, &u);
        assert_int_equal(u.records, i * 2U);
        assert_int_equal(unsetenv("VIGIL_TEST_FAULT"), 0);
        assert_int_equal(vigil_outbox_append(f->o, &c, &a), VIGIL_STORE_OK);
        assert_int_equal(a.order_id, i + 1U);
    }
    sqlite3 *db = inspect(f);
    assert_int_equal(number(db, "SELECT pending_records FROM spool_meta"), 14);
    assert_true(number(db, "SELECT recovery_generation FROM spool_meta") >= 3);
    assert_int_equal(sqlite3_close(db), SQLITE_OK);
}
static void failed_checkpoint_freezes_admission(void **state)
{
    struct fixture *f = *state;
    struct vigil_capture c = capture(1);
    struct vigil_local_commit commit;
    assert_int_equal(vigil_outbox_append(f->o, &c, &commit), VIGIL_STORE_OK);
    assert_int_equal(setenv("VIGIL_TEST_FAULT", "checkpoint_busy", 1), 0);
    assert_int_equal(vigil_outbox_checkpoint(f->o, false), VIGIL_STORE_BUSY);
    /* Below the size trigger, a failed checkpoint still freezes admission.
     * This also guards the shutdown caller leaving its checkpoint retry loop. */
    assert_int_equal(vigil_outbox_append(f->o, &c, &commit), VIGIL_STORE_BUSY);
    struct vigil_outbox_usage usage;
    vigil_outbox_usage(f->o, &usage);
    assert_int_equal(usage.records, 1);
    assert_int_equal(unsetenv("VIGIL_TEST_FAULT"), 0);
    assert_int_equal(vigil_outbox_append(f->o, &c, &commit), VIGIL_STORE_OK);
    assert_int_equal(commit.order_id, 2);
    assert_int_equal(commit.first_seq, 2);
    vigil_outbox_usage(f->o, &usage);
    assert_int_equal(usage.records, 2);
}
static void invalid_records(void **state)
{
    struct fixture *f = *state;
    struct vigil_capture c = capture(1);
    struct vigil_local_commit a;
    c.records[0].utc_ms = UINT64_MAX;
    assert_int_equal(vigil_outbox_append(f->o, &c, &a), VIGIL_STORE_INVALID);
    c = capture(1);
    (void)snprintf(c.records[0].detail, sizeof(c.records[0].detail), "{\"a\":1,\"a\":2}");
    assert_int_equal(vigil_outbox_append(f->o, &c, &a), VIGIL_STORE_INVALID);
    c = capture(1);
    (void)snprintf(c.records[0].detail, sizeof(c.records[0].detail),
                   "{\"transitions\":18446744073709551615}");
    assert_int_equal(vigil_outbox_append(f->o, &c, &a), VIGIL_STORE_INVALID);
    c = capture(1);
    c.stats.first_drop_ms = UINT64_MAX;
    assert_int_equal(vigil_outbox_append(f->o, &c, &a), VIGIL_STORE_INVALID);
    c = capture(1);
    c.count = 33;
    assert_int_equal(vigil_outbox_append(f->o, &c, &a), VIGIL_STORE_INVALID);
    c = capture(1);
    c.alarm_count = 0;
    assert_int_equal(vigil_outbox_append(f->o, &c, &a), VIGIL_STORE_INVALID);
    c = capture(1);
    assert_int_equal(vigil_outbox_append(f->o, &c, &a), VIGIL_STORE_OK);
    assert_int_equal(a.first_seq, 1);
    assert_int_equal(a.order_id, 1);
}
static void invalid_store(void **state)
{
    struct fixture *f = *state;
    struct vigil_outbox *other = NULL;
    assert_int_equal(vigil_outbox_open(&other, "relative", false), VIGIL_STORE_INVALID);
    assert_int_equal(vigil_outbox_open(&other, f->path, false), VIGIL_STORE_BUSY);
    sqlite3 *db = inspect(f);
    assert_int_equal(sqlite3_exec(db, "PRAGMA user_version=2", NULL, NULL, NULL), SQLITE_OK);
    assert_int_equal(sqlite3_close(db), SQLITE_OK);
    assert_int_equal(vigil_outbox_open(&other, f->path, false), VIGIL_STORE_CORRUPT);
    assert_null(other);
}
static void corrupt_accounting(void **state)
{
    struct fixture *f = *state;
    sqlite3 *db = inspect(f);
    assert_int_equal(sqlite3_exec(db, "UPDATE spool_meta SET next_order_id=2", NULL, NULL, NULL),
                     SQLITE_OK);
    assert_int_equal(sqlite3_close(db), SQLITE_OK);
    assert_int_equal(vigil_outbox_open(&f->o, f->path, false), VIGIL_STORE_CORRUPT);
}
static void maximum_payload(void **state)
{
    struct fixture *f = *state;
    struct vigil_capture c = capture(32);
    c.alarm_count = 193;
    char detail[384];
    memset(detail, 'x', sizeof(detail));
    memcpy(detail, "{\"pad\":\"", 8);
    detail[380] = '"';
    detail[381] = '}';
    detail[382] = '\0';
    for (size_t i = 0; i < 32; ++i)
        memcpy(c.records[i].detail, detail, sizeof(detail));
    for (size_t i = 1; i < 193; ++i)
        c.alarms[i] = c.alarms[0];
    for (size_t i = 0; i < 193; ++i)
        memcpy(c.alarms[i].detail, detail, sizeof(detail));
    struct vigil_local_commit a;
    assert_int_equal(vigil_outbox_append(f->o, &c, &a), VIGIL_STORE_OK);
    struct vigil_outbox_usage u;
    vigil_outbox_usage(f->o, &u);
    assert_true(u.bytes > 100000);
    assert_true(u.bytes <= 160000);
    assert_int_equal(vigil_outbox_checkpoint(f->o, true), VIGIL_STORE_OK);
}
static void bounded_handoff_shutdown(void **state)
{
    struct fixture *f = *state;
    assert_int_equal(vigil_outbox_close(f->o), VIGIL_STORE_OK);
    f->o = NULL;
    struct vigil_persistence p;
    assert_int_equal(setenv("VIGIL_DB_HOST", "/tmp/vigil-test-no-postgres", 1),0);
    assert_true(vigil_persistence_start(&p, 2, f->path));
    struct vigil_capture c = capture(1);
    for (unsigned i = 0; i < 11; ++i)
        assert_true(vigil_queue_push(&p.queue, c.records[0]) <= VIGIL_QUEUED_EVICTED);
    struct vigil_queue_stats s;
    vigil_queue_stats(&p.queue, &s);
    assert_int_equal(s.readings_dropped, 9); /* ingestion waits for initial snapshot */
    vigil_queue_publish(&p.queue, c.alarms, 1, 1);
    uint64_t start = vigil_now_ms();
    vigil_persistence_stop(&p);
    assert_true(vigil_now_ms() - start < 1250);
    vigil_queue_stats(&p.queue, &s);
    assert_int_equal(s.locally_committed_readings, 2);
    assert_int_equal(s.readings_queued, s.readings_evicted + s.locally_committed_readings +
                                            s.reading_depth + s.in_flight_readings);
    assert_int_equal(s.records_written, 0);
    assert_int_equal(s.spool_retired, 0);
    assert_int_equal(s.abandoned_readings, 0);
    assert_int_equal(vigil_queue_push(&p.queue, c.records[0]), VIGIL_CLOSED);
    vigil_persistence_destroy(&p);
}
static void wal_growth(void **state)
{
    struct fixture *f = *state;
    struct vigil_capture c = capture(32);
    c.alarm_count = 193;
    char detail[384];
    memset(detail, 'x', sizeof(detail));
    memcpy(detail, "{\"pad\":\"", 8);
    detail[380] = '"';
    detail[381] = '}';
    detail[382] = '\0';
    for (size_t i = 0; i < 32; ++i)
        memcpy(c.records[i].detail, detail, sizeof(detail));
    for (size_t i = 0; i < 193; ++i) {
        c.alarms[i] = c.alarms[0];
        memcpy(c.alarms[i].detail, detail, sizeof(detail));
    }
    off_t max_wal = 0;
    char wal[140];
    (void)snprintf(wal, sizeof(wal), "%s-wal", f->path);
    struct vigil_local_commit commit;
    for (unsigned i = 0; i < 48; ++i) {
        ++c.revision;
        assert_int_equal(vigil_outbox_append(f->o, &c, &commit), VIGIL_STORE_OK);
        struct stat st;
        assert_int_equal(stat(wal, &st), 0);
        if (st.st_size > max_wal) max_wal = st.st_size;
        assert_true(st.st_size < 5 * 1024 * 1024); /* trigger plus one bounded transaction */
    }
    assert_true(max_wal > 4 * 1024 * 1024);
    assert_int_equal(vigil_outbox_close(f->o), VIGIL_STORE_OK);
    f->o = NULL;
    assert_int_equal(vigil_outbox_open(&f->o, f->path, false), VIGIL_STORE_OK);
    struct vigil_outbox_usage u;
    vigil_outbox_usage(f->o, &u);
    assert_int_equal(u.records, 48 * 32);
    assert_true(u.bytes > 4 * 1024 * 1024);
}
static void origins_and_exhaustion(void **state)
{
    struct fixture *f = *state;
    for (unsigned i = 0; i < 20; ++i) {
        assert_int_equal(vigil_outbox_close(f->o), VIGIL_STORE_OK);
        f->o = NULL;
        assert_int_equal(vigil_outbox_open(&f->o, f->path, false), VIGIL_STORE_OK);
        assert_int_equal(vigil_outbox_begin_run(f->o), VIGIL_STORE_OK);
    }
    sqlite3 *db = inspect(f);
    assert_int_equal(number(db, "SELECT count(*) FROM origin_runs"), 1);
    assert_int_equal(number(db, "SELECT next_run_no FROM spool_meta"), 22);
    assert_int_equal(
        sqlite3_exec(db, "UPDATE spool_meta SET next_run_no=9223372036854775807", NULL, NULL, NULL),
        SQLITE_OK);
    assert_int_equal(sqlite3_close(db), SQLITE_OK);
    assert_int_equal(vigil_outbox_open(&f->o, f->path, false), VIGIL_STORE_OK);
    assert_int_equal(vigil_outbox_begin_run(f->o), VIGIL_STORE_EXHAUSTED);
}
static void priority_ticket_exhaustion(void **state)
{
    (void)state;
    struct vigil_queue q;
    assert_true(vigil_queue_init(&q, 1));
    struct vigil_capture c = capture(2);
    assert_int_equal(vigil_queue_push(&q, c.records[1]), VIGIL_QUEUED);
    for (unsigned i = 0; i < 5; ++i)
        (void)vigil_queue_push(&q, c.records[0]);
    vigil_queue_take_local(&q, &c);
    assert_int_equal(c.count, 2);
    assert_string_equal(c.records[0].kind, "heartbeat");
    assert_int_equal(c.stats.readings_evicted, 4);
    assert_int_equal(c.stats.events_evicted, 0);
    q.next_seq = (uint64_t)INT64_MAX - 1U;
    assert_int_equal(vigil_queue_push(&q, c.records[0]), VIGIL_RESOURCE_REJECTED);
    assert_int_equal(q.stats.events_rejected, 1);
    vigil_queue_destroy(&q);
}
static void retire_fifo_reopen(void **state)
{
    struct fixture *f=*state;
    struct vigil_capture c=capture(2);
    struct vigil_local_commit a;
    struct vigil_replay_batch *b=calloc(1,sizeof(*b));
    assert_non_null(b);
    assert_int_equal(vigil_outbox_append(f->o,&c,&a),VIGIL_STORE_OK);
    c.revision=2; c.stats.readings_dropped=3; c.stats.readings_evicted=3;
    assert_int_equal(vigil_outbox_append(f->o,&c,&a),VIGIL_STORE_OK);
    assert_int_equal(vigil_outbox_peek(f->o,b),VIGIL_STORE_OK);
    assert_int_equal(b->id.order_id,1);
    assert_int_equal(setenv("VIGIL_TEST_FAULT","retire_rollback",1),0);
    assert_int_equal(vigil_outbox_retire(f->o,b),VIGIL_STORE_FULL);
    assert_int_equal(unsetenv("VIGIL_TEST_FAULT"),0);
    struct vigil_outbox_usage u;
    vigil_outbox_usage(f->o,&u); assert_int_equal(u.retired,0); assert_int_equal(u.records,4);
    assert_int_equal(vigil_outbox_retire(f->o,b),VIGIL_STORE_OK);
    assert_int_equal(vigil_outbox_close(f->o),VIGIL_STORE_OK); f->o=NULL;
    assert_int_equal(vigil_outbox_open(&f->o,f->path,false),VIGIL_STORE_OK);
    assert_int_equal(vigil_outbox_begin_run(f->o),VIGIL_STORE_OK);
    assert_int_equal(vigil_outbox_peek(f->o,b),VIGIL_STORE_OK); assert_int_equal(b->id.order_id,2);
    assert_int_equal(b->revision,2); assert_int_equal(b->reading_drops,3);
    assert_int_equal(vigil_outbox_retire(f->o,b),VIGIL_STORE_OK);
    assert_int_equal(vigil_outbox_peek(f->o,b),VIGIL_STORE_OK); assert_int_equal(b->bytes,0);
    sqlite3 *db=inspect(f);
    assert_int_equal(number(db,"SELECT admitted_records FROM spool_meta"),4);
    assert_int_equal(number(db,"SELECT retired_records FROM spool_meta"),4);
    assert_int_equal(number(db,"SELECT last_retired_order FROM spool_meta"),2);
    assert_int_equal(number(db,"SELECT count(*) FROM origin_runs"),1);
    assert_int_equal(sqlite3_close(db),SQLITE_OK);
    assert_int_equal(vigil_outbox_open(&f->o,f->path,false),VIGIL_STORE_OK);
    free(b);
}
static void retire_uncertainty_with_held(void **state)
{
    struct fixture *f=*state;
    struct vigil_capture c=capture(2);
    struct vigil_local_commit a;
    struct vigil_replay_batch *b=calloc(1,sizeof(*b)); assert_non_null(b);
    assert_int_equal(setenv("VIGIL_TEST_RECORD_LIMIT","2",1),0);
    assert_int_equal(vigil_outbox_append(f->o,&c,&a),VIGIL_STORE_OK);
    assert_int_equal(vigil_outbox_append(f->o,&c,&a),VIGIL_STORE_FULL); /* pinned candidate */
    assert_int_equal(vigil_outbox_peek(f->o,b),VIGIL_STORE_OK);
    assert_int_equal(setenv("VIGIL_TEST_FAULT","retire_commit_unknown",1),0);
    assert_int_equal(vigil_outbox_retire(f->o,b),VIGIL_STORE_IO_UNKNOWN);
    assert_int_equal(unsetenv("VIGIL_TEST_FAULT"),0);
    assert_int_equal(vigil_outbox_retire(f->o,b),VIGIL_STORE_OK);
    struct vigil_outbox_usage u; vigil_outbox_usage(f->o,&u);
    assert_int_equal(u.retired,2); assert_int_equal(u.records,0);
    assert_int_equal(vigil_outbox_append(f->o,&c,&a),VIGIL_STORE_OK);
    assert_int_equal(a.order_id,2); assert_int_equal(a.first_seq,3);
    assert_int_equal(vigil_outbox_peek(f->o,b),VIGIL_STORE_OK);
    assert_int_equal(vigil_outbox_retire(f->o,b),VIGIL_STORE_OK);
    assert_int_equal(vigil_outbox_close(f->o),VIGIL_STORE_OK); f->o=NULL;
    assert_int_equal(vigil_outbox_open(&f->o,f->path,false),VIGIL_STORE_OK);
    free(b);
}
static void retire_checkpoint_freeze(void **state)
{
    struct fixture *f=*state;
    struct vigil_capture c=capture(2); struct vigil_local_commit a;
    struct vigil_replay_batch *b=calloc(1,sizeof(*b)); assert_non_null(b);
    assert_int_equal(vigil_outbox_append(f->o,&c,&a),VIGIL_STORE_OK);
    assert_int_equal(vigil_outbox_peek(f->o,b),VIGIL_STORE_OK);
    assert_int_equal(setenv("VIGIL_TEST_FAULT","checkpoint_busy",1),0);
    assert_int_equal(vigil_outbox_checkpoint(f->o,false),VIGIL_STORE_BUSY);
    assert_int_equal(vigil_outbox_retire(f->o,b),VIGIL_STORE_BUSY);
    assert_int_equal(unsetenv("VIGIL_TEST_FAULT"),0);
    assert_int_equal(vigil_outbox_retire(f->o,b),VIGIL_STORE_OK);
    free(b);
}
static void retire_storage_faults(void **state)
{
    struct fixture *f=*state;
    struct vigil_capture c=capture(2); struct vigil_local_commit a;
    struct vigil_replay_batch *b=calloc(1,sizeof(*b)); assert_non_null(b);
    const char *faults[]={"write_full","write_readonly","lock_busy","write_io","sync_io","directory_sync"};
    const enum vigil_store_result expected[]={VIGIL_STORE_FULL,VIGIL_STORE_READONLY,VIGIL_STORE_BUSY,VIGIL_STORE_IO_UNKNOWN,VIGIL_STORE_IO_UNKNOWN,VIGIL_STORE_IO_UNKNOWN};
    for (size_t i=0;i<6;++i) {
        assert_int_equal(vigil_outbox_append(f->o,&c,&a),VIGIL_STORE_OK);
        assert_int_equal(vigil_outbox_peek(f->o,b),VIGIL_STORE_OK);
        assert_int_equal(setenv("VIGIL_TEST_FAULT",faults[i],1),0);
        assert_int_equal(vigil_outbox_retire(f->o,b),expected[i]);
        struct vigil_outbox_usage u; vigil_outbox_usage(f->o,&u);
        assert_int_equal(u.retired,i*2U); /* failed call must publish none */
        assert_int_equal(unsetenv("VIGIL_TEST_FAULT"),0);
        assert_int_equal(vigil_outbox_retire(f->o,b),VIGIL_STORE_OK);
        assert_int_equal(vigil_outbox_close(f->o),VIGIL_STORE_OK); f->o=NULL;
        assert_int_equal(vigil_outbox_open(&f->o,f->path,false),VIGIL_STORE_OK);
        assert_int_equal(vigil_outbox_begin_run(f->o),VIGIL_STORE_OK);
        vigil_outbox_usage(f->o,&u); assert_int_equal(u.retired,(i+1U)*2U);
        assert_int_equal(u.records,0);
    }
    free(b);
}
static void retire_refuses_conflict(void **state)
{
    struct fixture *f=*state;
    struct vigil_capture c=capture(2); struct vigil_local_commit a;
    struct vigil_replay_batch *b=calloc(1,sizeof(*b)); assert_non_null(b);
    assert_int_equal(vigil_outbox_append(f->o,&c,&a),VIGIL_STORE_OK);
    assert_int_equal(vigil_outbox_peek(f->o,b),VIGIL_STORE_OK);
    ++b->id.order_id;
    assert_int_equal(vigil_outbox_retire(f->o,b),VIGIL_STORE_CORRUPT);
    --b->id.order_id;
    b->payload[0]='x';
    assert_int_equal(vigil_outbox_retire(f->o,b),VIGIL_STORE_CORRUPT);
    b->payload[0]='{';
    assert_int_equal(vigil_outbox_retire(f->o,b),VIGIL_STORE_OK);
    free(b);
}
int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--handoff-only") == 0)
        cmocka_set_test_filter("bounded_handoff_shutdown");
    else if (argc != 1)
        return 2;
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(retire_storage_faults, setup, teardown),
        cmocka_unit_test_setup_teardown(retire_refuses_conflict, setup, teardown),
        cmocka_unit_test_setup_teardown(retire_fifo_reopen, setup, teardown),
        cmocka_unit_test_setup_teardown(retire_uncertainty_with_held, setup, teardown),
        cmocka_unit_test_setup_teardown(retire_checkpoint_freeze, setup, teardown),
        cmocka_unit_test_setup_teardown(identities_order_reopen, setup, teardown),
        cmocka_unit_test_setup_teardown(atomic_rollback, setup, teardown),
        cmocka_unit_test_setup_teardown(capacity_records, setup, teardown),
        cmocka_unit_test_setup_teardown(capacity_batches, setup, teardown),
        cmocka_unit_test_setup_teardown(capacity_bytes, setup, teardown),
        cmocka_unit_test_setup_teardown(failed_writes, setup, teardown),
        cmocka_unit_test_setup_teardown(failed_checkpoint_freezes_admission, setup, teardown),
        cmocka_unit_test_setup_teardown(invalid_records, setup, teardown),
        cmocka_unit_test_setup_teardown(invalid_store, setup, teardown),
        cmocka_unit_test_setup_teardown(corrupt_accounting, setup, teardown),
        cmocka_unit_test_setup_teardown(maximum_payload, setup, teardown),
        cmocka_unit_test_setup_teardown(bounded_handoff_shutdown, setup, teardown),
        cmocka_unit_test_setup_teardown(wal_growth, setup, teardown),
        cmocka_unit_test_setup_teardown(origins_and_exhaustion, setup, teardown),
        cmocka_unit_test(priority_ticket_exhaustion)};
    return cmocka_run_group_tests(tests, NULL, NULL);
}
