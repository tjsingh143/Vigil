#include "vigil/outbox.h"
#include "vigil/failpoint.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define ID_MAX (INT64_MAX - 1)
#define UTC_MAX INT64_C(253402300799999)
#define WAL_TRIGGER (4U * 1024U * 1024U)
#define WAL_MAX (520U * 1024U * 1024U)
struct vigil_outbox {
    sqlite3 *db;
    pthread_t owner;
    int dir, lock, error;
    char path[PATH_MAX], base[NAME_MAX + 1];
    char spool[37], run[37], token[65], boot[37];
    int64_t run_no, started, utc, next_seq, next_batch, next_order;
    uint64_t captured_readings, captured_events, revision;
    uint64_t pending_reading_drops, pending_event_drops, pending_revision;
    struct vigil_outbox_usage usage;
    struct vigil_local_commit candidate;
    char payload[VIGIL_PAYLOAD_MAX + 1U];
    size_t used;
    bool pending, uncertain, json_ok, recovering, checkpoint_blocked;
    uint64_t validation_deadline;
};
static const char *const schema[] = {
    ("CREATE TABLE spool_meta("
     "id INTEGER PRIMARY KEY CHECK(id=1),spool_id TEXT NOT NULL UNIQUE CHECK(length(spool_id)=36 "
     "AND substr(spool_id,9,1)='-' AND substr(spool_id,14,1)='-' AND substr(spool_id,19,1)='-' AND "
     "substr(spool_id,24,1)='-' AND length(replace(spool_id,'-',''))=32 AND "
     "replace(spool_id,'-','') NOT GLOB '*[^0-9a-f]*'),"
     "next_run_no INTEGER NOT NULL CHECK(next_run_no>=1),next_order_id INTEGER NOT NULL "
     "CHECK(next_order_id>=1),"
     "last_retired_order INTEGER NOT NULL CHECK(last_retired_order>=0 AND "
     "last_retired_order<next_order_id),"
     "recovery_generation INTEGER NOT NULL CHECK(recovery_generation>=0),"
     "pending_records INTEGER NOT NULL CHECK(pending_records BETWEEN 0 AND 65536),"
     "pending_batches INTEGER NOT NULL CHECK(pending_batches BETWEEN 0 AND 4096),"
     "pending_payload_bytes INTEGER NOT NULL CHECK(pending_payload_bytes BETWEEN 0 AND 67108864),"
     "admitted_records INTEGER NOT NULL CHECK(admitted_records>=0),admitted_readings INTEGER NOT "
     "NULL CHECK(admitted_readings BETWEEN 0 AND admitted_records),"
     "retired_records INTEGER NOT NULL CHECK(retired_records BETWEEN 0 AND admitted_records),"
     "retired_readings INTEGER NOT NULL CHECK(retired_readings BETWEEN 0 AND "
     "min(retired_records,admitted_readings)),"
     "CHECK(admitted_records-retired_records=pending_records)) STRICT;"),
    ("CREATE TABLE origin_runs("
     "run_id TEXT PRIMARY KEY NOT NULL CHECK(length(run_id)=36 AND substr(run_id,9,1)='-' AND "
     "substr(run_id,14,1)='-' AND substr(run_id,19,1)='-' AND substr(run_id,24,1)='-' AND "
     "length(replace(run_id,'-',''))=32 AND replace(run_id,'-','') NOT GLOB '*[^0-9a-f]*'),run_no "
     "INTEGER NOT NULL UNIQUE CHECK(run_no BETWEEN 1 AND 9223372036854775806),"
     "run_token TEXT NOT NULL UNIQUE CHECK(length(run_token)=64 AND run_token NOT GLOB "
     "'*[^0-9a-f]*'),"
     "boot_id TEXT NOT NULL CHECK(length(boot_id)=36 AND substr(boot_id,9,1)='-' AND "
     "substr(boot_id,14,1)='-' AND substr(boot_id,19,1)='-' AND substr(boot_id,24,1)='-' AND "
     "length(replace(boot_id,'-',''))=32 AND replace(boot_id,'-','') NOT GLOB "
     "'*[^0-9a-f]*'),started_utc_ms INTEGER NOT NULL CHECK(started_utc_ms BETWEEN 0 AND "
     "253402300799999),"
     "started_ms INTEGER NOT NULL CHECK(started_ms>=0),next_seq INTEGER NOT NULL "
     "CHECK(next_seq>=1),next_batch_id INTEGER NOT NULL CHECK(next_batch_id>=1),"
     "admitted_records INTEGER NOT NULL CHECK(admitted_records>=0),admitted_readings INTEGER NOT "
     "NULL CHECK(admitted_readings BETWEEN 0 AND admitted_records),"
     "retired_records INTEGER NOT NULL CHECK(retired_records BETWEEN 0 AND admitted_records),"
     "retired_readings INTEGER NOT NULL CHECK(retired_readings BETWEEN 0 AND "
     "min(retired_records,admitted_readings)),"
     "captured_reading_drops INTEGER NOT NULL "
     "CHECK(captured_reading_drops>=0),captured_event_drops "
     "INTEGER NOT NULL CHECK(captured_event_drops>=0),"
     "captured_revision INTEGER NOT NULL CHECK(captured_revision>=0),"
     "retired_reading_drops INTEGER NOT NULL CHECK(retired_reading_drops BETWEEN 0 AND "
     "captured_reading_drops),"
     "retired_event_drops INTEGER NOT NULL CHECK(retired_event_drops BETWEEN 0 AND "
     "captured_event_drops),"
     "retired_revision INTEGER NOT NULL CHECK(retired_revision BETWEEN 0 AND captured_revision)) "
     "STRICT;"),
    ("CREATE TABLE outbox_batches("
     "order_id INTEGER PRIMARY KEY CHECK(order_id BETWEEN 1 AND 9223372036854775806),"
     "run_id TEXT NOT NULL REFERENCES origin_runs(run_id) ON DELETE RESTRICT,batch_id INTEGER NOT "
     "NULL CHECK(batch_id BETWEEN 1 AND 9223372036854775806),"
     "first_seq INTEGER,record_count INTEGER NOT NULL CHECK(record_count BETWEEN 0 AND 32),"
     "reading_count INTEGER NOT NULL CHECK(reading_count BETWEEN 0 AND record_count),payload TEXT "
     "NOT NULL,"
     "payload_bytes INTEGER NOT NULL CHECK(payload_bytes BETWEEN 1 AND 160000 AND "
     "payload_bytes=length(CAST(payload AS BLOB))),"
     "UNIQUE(run_id,batch_id),CHECK((record_count=0 AND first_seq IS NULL) OR (record_count>0 AND "
     "first_seq BETWEEN 1 AND 9223372036854775806-record_count+1)),"
     "CHECK(json_valid(payload)),CHECK(json_type(payload,'$.records') IS 'array' AND "
     "json_array_length(payload,'$.records')=record_count)) STRICT;"),
    ("CREATE TRIGGER immutable_origin BEFORE UPDATE OF "
     "run_id,run_no,run_token,boot_id,started_utc_ms,started_ms ON origin_runs BEGIN SELECT "
     "RAISE(ABORT,'immutable'); END;"),
    ("CREATE TRIGGER immutable_batch BEFORE UPDATE ON outbox_batches BEGIN SELECT "
     "RAISE(ABORT,'immutable'); END;"),
    ("PRAGMA application_id=1447642956;PRAGMA user_version=1;")};

static void owner(struct vigil_outbox *o)
{
    vigil_queue_assert_io();
    if (pthread_equal(o->owner, pthread_self()) == 0) abort();
}
static enum vigil_store_result result(struct vigil_outbox *o, int rc)
{
    o->error = rc;
    switch (rc & 255) {
    case SQLITE_OK:
        return VIGIL_STORE_OK;
    case SQLITE_FULL:
        return VIGIL_STORE_FULL;
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
        return VIGIL_STORE_BUSY;
    case SQLITE_READONLY:
        return VIGIL_STORE_READONLY;
    case SQLITE_NOMEM:
        return VIGIL_STORE_NOMEM;
    case SQLITE_IOERR:
    case SQLITE_CANTOPEN:
    case SQLITE_PROTOCOL:
        return VIGIL_STORE_IO_UNKNOWN;
    case SQLITE_CORRUPT:
    case SQLITE_NOTADB:
    case SQLITE_CONSTRAINT:
        return VIGIL_STORE_CORRUPT;
    default:
        return VIGIL_STORE_INVALID;
    }
}
static int exec_sql(struct vigil_outbox *o, const char *sql)
{
    owner(o);
    return sqlite3_exec(o->db, sql, NULL, NULL, NULL);
}
static int prepare(struct vigil_outbox *o, const char *sql, sqlite3_stmt **s)
{
    owner(o);
    return sqlite3_prepare_v2(o->db, sql, -1, s, NULL);
}
static int finish(sqlite3_stmt *s, int rc)
{
    int end = sqlite3_finalize(s);
    return rc == SQLITE_OK ? end : rc;
}
static int scalar(struct vigil_outbox *o, const char *sql, int64_t *out)
{
    sqlite3_stmt *s = NULL;
    int rc = prepare(o, sql, &s);
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(s);
        if (rc == SQLITE_ROW && sqlite3_column_type(s, 0) == SQLITE_INTEGER) {
            *out = sqlite3_column_int64(s, 0);
            rc = sqlite3_step(s);
            if (rc == SQLITE_DONE)
                rc = SQLITE_OK;
            else if (rc == SQLITE_ROW)
                rc = SQLITE_CORRUPT;
        } else if (rc == SQLITE_ROW || rc == SQLITE_DONE)
            rc = SQLITE_CORRUPT;
    }
    return finish(s, rc);
}
static int expect(struct vigil_outbox *o, const char *sql, int64_t expected)
{
    int64_t got = 0;
    int rc = scalar(o, sql, &got);
    return rc == SQLITE_OK && got != expected ? SQLITE_CORRUPT : rc;
}
static bool uuid(const char *s)
{
    if (strlen(s) != 36) return false;
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-') return false;
        } else if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return false;
    }
    return true;
}
static bool random_hex(char *out, size_t bytes, bool make_uuid)
{
    unsigned char b[32];
    size_t have = 0;
    while (have < bytes) {
        ssize_t n = getrandom(b + have, bytes - have, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        have += (size_t)n;
    }
    if (make_uuid) {
        b[6] = (unsigned char)((b[6] & 15U) | 64U);
        b[8] = (unsigned char)((b[8] & 63U) | 128U);
    }
    size_t pos = 0;
    const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < bytes; ++i) {
        if (make_uuid && (i == 4 || i == 6 || i == 8 || i == 10)) out[pos++] = '-';
        out[pos++] = hex[b[i] >> 4];
        out[pos++] = hex[b[i] & 15U];
    }
    out[pos] = '\0';
    return true;
}
static int directory_sync(struct vigil_outbox *o)
{
    owner(o);
    vigil_test_hook("before_directory_sync");
    if (vigil_test_fault("directory_sync") || fsync(o->dir) != 0) return SQLITE_IOERR_FSYNC;
    vigil_test_hook("after_directory_sync");
    return SQLITE_OK;
}
static int rollback(struct vigil_outbox *o, int rc)
{
    if (sqlite3_get_autocommit(o->db) == 0 && exec_sql(o, "ROLLBACK") != SQLITE_OK)
        rc = SQLITE_IOERR_ROLLBACK_ATOMIC;
    if (sqlite3_get_autocommit(o->db) == 0) rc = SQLITE_IOERR;
    return rc;
}
static int file_check(struct vigil_outbox *o, const char *suffix, uint64_t max, bool required)
{
    char name[NAME_MAX + 8];
    int n = snprintf(name, sizeof(name), "%s%s", o->base, suffix);
    if (n < 0 || (size_t)n >= sizeof(name)) return SQLITE_CANTOPEN;
    struct stat st;
    if (fstatat(o->dir, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
        return errno == ENOENT && !required ? SQLITE_OK : SQLITE_CANTOPEN;
    if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0777) != 0600 ||
        st.st_nlink != 1 || st.st_size < 0 || (uint64_t)st.st_size > max)
        return SQLITE_CORRUPT;
    return SQLITE_OK;
}
static int files_check(struct vigil_outbox *o)
{
    int rc = file_check(o, "", UINT64_C(512) * 1024U * 1024U, true);
    if (rc == SQLITE_OK) rc = file_check(o, "-wal", WAL_MAX - 1U, false);
    if (rc == SQLITE_OK) rc = file_check(o, "-shm", 2U * 1024U * 1024U, false);
    return rc;
}
static int settings(struct vigil_outbox *o)
{
    int rc = exec_sql(o, "PRAGMA journal_mode=WAL;PRAGMA synchronous=FULL;PRAGMA foreign_keys=ON;"
                         "PRAGMA max_page_count=131072;PRAGMA wal_autocheckpoint=0;PRAGMA "
                         "journal_size_limit=4194304;"
                         "PRAGMA cache_size=-2048;PRAGMA cache_spill=OFF;PRAGMA mmap_size=0;PRAGMA "
                         "temp_store=MEMORY;PRAGMA busy_timeout=0;");
    static const struct {
        const char *sql;
        int64_t value;
    } checks[] = {{"SELECT count(*) FROM pragma_journal_mode WHERE journal_mode='wal'", 1},
                  {"PRAGMA synchronous", 2},
                  {"PRAGMA foreign_keys", 1},
                  {"PRAGMA page_size", 4096},
                  {"PRAGMA max_page_count", 131072},
                  {"PRAGMA wal_autocheckpoint", 0},
                  {"PRAGMA journal_size_limit", 4194304},
                  {"PRAGMA cache_size", -2048},
                  {"PRAGMA cache_spill", 0},
                  {"PRAGMA mmap_size", 0},
                  {"PRAGMA temp_store", 2},
                  {"PRAGMA busy_timeout", 0}};
    for (size_t i = 0; rc == SQLITE_OK && i < sizeof(checks) / sizeof(checks[0]); ++i)
        rc = expect(o, checks[i].sql, checks[i].value);
    return rc;
}
static int connect_store(struct vigil_outbox *o, bool fresh)
{
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_NOFOLLOW;

    vigil_test_vfs_install();
    int rc = sqlite3_open_v2(o->path, &o->db, flags, NULL);
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_extended_result_codes(o->db, 1);
    if (rc != SQLITE_OK) return rc;
    (void)sqlite3_limit(o->db, SQLITE_LIMIT_LENGTH, 192 * 1024);
    (void)sqlite3_limit(o->db, SQLITE_LIMIT_SQL_LENGTH, 64 * 1024);
    (void)sqlite3_limit(o->db, SQLITE_LIMIT_ATTACHED, 0);
    if (!fresh && rc == SQLITE_OK) rc = expect(o, "PRAGMA user_version", 1);
    if (!fresh && rc == SQLITE_OK) rc = expect(o, "PRAGMA application_id", 1447642956);
    if (fresh && rc == SQLITE_OK) rc = exec_sql(o, "PRAGMA page_size=4096");
    return rc == SQLITE_OK ? settings(o) : rc;
}
static int deadline(void *ctx)
{
    return vigil_now_ms() > ((struct vigil_outbox *)ctx)->validation_deadline;
}

static int payload_metadata_valid(struct vigil_outbox *o, const char *payload, size_t bytes)
{
    const char *sql =
        "SELECT json_type(?1,'$.origin') IS 'object' "
        "AND json_type(?1,'$.format') IS 'integer' AND json_type(?1,'$.order_id') IS 'integer' "
        "AND json_type(?1,'$.batch_id') IS 'integer' "
        "AND json_type(?1,'$.origin.run_no') IS 'integer' "
        "AND json_type(?1,'$.origin.started_ms') IS 'integer' "
        "AND json_type(?1,'$.origin.started_utc_ms') IS 'integer' "
        "AND json_type(?1,'$.started_ms') IS 'integer' "
        "AND json_type(?1,'$.started_utc_ms') IS 'integer' "
        "AND NOT EXISTS(SELECT 1 FROM json_tree(?1) WHERE typeof(atom)='real') "
        "AND (SELECT count(*) FROM json_each(?1,'$.health'))=14 "
        "AND NOT EXISTS(SELECT 1 FROM json_each(?1,'$.health') WHERE type!='integer' "
        "OR atom NOT BETWEEN 0 AND 9223372036854775806 OR key NOT IN ("
        "'readings_queued','events_queued','readings_dropped','events_dropped','first_drop_ms',"
        "'last_drop_ms','revision','outage_started_ms','outage_ms','outage_total_ms','outage_ended_"
        "ms',"
        "'connection_failures','query_failures','reconnects')) "
        "AND (SELECT count(*) FROM json_each(?1,'$.local_loss'))=4 "
        "AND NOT EXISTS(SELECT 1 FROM json_each(?1,'$.local_loss') WHERE type!='integer' "
        "OR atom NOT BETWEEN 0 AND 9223372036854775806 OR key NOT IN ("
        "'reading_evicted','event_evicted','reading_rejected','event_rejected')) "
        "AND json_extract(?1,'$.health.revision') IS json_extract(?1,'$.revision') "
        "AND "
        "json_extract(?1,'$.local_loss.reading_rejected')<=json_extract(?1,'$.health.readings_"
        "dropped') "
        "AND "
        "json_extract(?1,'$.local_loss.event_rejected')<=json_extract(?1,'$.health.events_dropped')"
        " "
        "AND json_extract(?1,'$.local_loss.reading_evicted') IS "
        "(json_extract(?1,'$.health.readings_dropped')-json_extract(?1,'$.local_loss.reading_"
        "rejected')) "
        "AND json_extract(?1,'$.local_loss.event_evicted') IS "
        "(json_extract(?1,'$.health.events_dropped')-json_extract(?1,'$.local_loss.event_rejected')"
        ")";
    sqlite3_stmt *stmt = NULL;
    int rc = prepare(o, sql, &stmt);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, payload, (int)bytes, SQLITE_STATIC);
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW)
            rc = sqlite3_column_int(stmt, 0) == 1 ? SQLITE_OK : SQLITE_CORRUPT;
        else if (rc == SQLITE_DONE)
            rc = SQLITE_CORRUPT;
    }
    return finish(stmt, rc);
}

static bool utf8(const unsigned char *s);

/* All payloads, including recovered rows, pass the same JSON predicates. SQL
 * uses IS/COALESCE so missing and null required fields cannot evade validation. */
static int payload_valid(struct vigil_outbox *o, const char *payload, size_t bytes, int64_t order,
                         const char *run, int64_t batch, int64_t first, int64_t count,
                         int64_t readings)
{
    if (bytes == 0 || bytes > VIGIL_PAYLOAD_MAX || memchr(payload, 0, bytes) != NULL ||
        !utf8((const unsigned char *)payload)) return SQLITE_CORRUPT;
    sqlite3_stmt *s = NULL;
    const char *sql =
        "SELECT json_valid(?1) AND json_type(?1) IS 'object' "
        "AND json_extract(?1,'$.format') IS 2 AND json_extract(?1,'$.order_id') IS ?2 "
        "AND json_extract(?1,'$.run_id') IS ?3 AND json_extract(?1,'$.batch_id') IS ?4 "
        "AND json_type(?1,'$.records') IS 'array' AND json_array_length(?1,'$.records') IS ?6 "
        "AND json_extract(?1,'$.origin.spool_id') IS m.spool_id "
        "AND json_extract(?1,'$.origin.run_id') IS r.run_id "
        "AND json_extract(?1,'$.origin.run_no') IS r.run_no "
        "AND json_extract(?1,'$.origin.run_token') IS r.run_token "
        "AND json_extract(?1,'$.origin.boot_id') IS r.boot_id "
        "AND json_extract(?1,'$.origin.started_ms') IS r.started_ms "
        "AND json_extract(?1,'$.origin.started_utc_ms') IS r.started_utc_ms "
        "AND json_extract(?1,'$.started_ms') IS r.started_ms "
        "AND json_extract(?1,'$.started_utc_ms') IS r.started_utc_ms "
        "AND json_type(?1,'$.mono_ms') IS 'integer' AND json_extract(?1,'$.mono_ms')>=0 "
        "AND json_type(?1,'$.utc_ms') IS 'integer' AND json_extract(?1,'$.utc_ms') BETWEEN 0 AND "
        "253402300799999 "
        "AND json_type(?1,'$.capture_ticket') IS 'integer' AND "
        "json_extract(?1,'$.capture_ticket')>=0 "
        "AND json_type(?1,'$.revision') IS 'integer' AND json_extract(?1,'$.revision')>=0 "
        "AND json_extract(?1,'$.kind') IN ('flush','gap','recovery') "
        "AND json_type(?1,'$.reading_drops') IS 'integer' AND "
        "json_extract(?1,'$.reading_drops')>=0 "
        "AND json_type(?1,'$.event_drops') IS 'integer' AND json_extract(?1,'$.event_drops')>=0 "
        "AND json_type(?1,'$.health') IS 'object' AND json_type(?1,'$.local_loss') IS 'object' "
        "AND (json_type(?1,'$.alarms') IS 'null' OR (json_type(?1,'$.alarms') IS 'array' "
        "AND json_array_length(?1,'$.alarms') BETWEEN 1 AND 193)) "
        "AND (SELECT count(*) FROM json_each(?1,'$.records') WHERE "
        "json_extract(value,'$.kind')='reading') IS ?7 "
        "AND NOT EXISTS(SELECT 1 FROM json_each(?1,'$.records') WHERE "
        "json_type(value) IS NOT 'object' OR json_type(value,'$.seq') IS NOT 'integer' OR "
        "json_extract(value,'$.seq') IS NOT (?5+CAST(key AS INTEGER)) "
        "OR json_type(value,'$.mono_ms') IS NOT 'integer' OR json_extract(value,'$.mono_ms')<0 "
        "OR json_type(value,'$.utc_ms') IS NOT 'integer' OR json_extract(value,'$.utc_ms') NOT "
        "BETWEEN 0 AND 253402300799999 "
        "OR json_type(value,'$.pod') IS NOT 'integer' OR json_extract(value,'$.pod') NOT BETWEEN 0 "
        "AND 127 "
        "OR json_type(value,'$.channel') IS NOT 'integer' OR json_extract(value,'$.channel') NOT "
        "BETWEEN -3 AND 1 "
        "OR coalesce(json_extract(value,'$.kind') IN "
        "('reading','heartbeat','rejected','alarm'),0)=0 "
        "OR json_type(value,'$.detail') IS NOT 'object' OR "
        "length(CAST(json_extract(value,'$.detail') AS BLOB))>=384) "
        "AND NOT EXISTS(SELECT 1 FROM json_each(?1,'$.alarms') WHERE "
        "json_type(?1,'$.alarms')='array' AND ("
        "json_type(value) IS NOT 'object' OR json_type(value,'$.seq') IS NOT 'integer' OR "
        "json_extract(value,'$.seq') IS NOT 0 "
        "OR json_extract(value,'$.kind') IS NOT 'alarm' OR json_type(value,'$.detail') IS NOT "
        "'object' "
        "OR length(CAST(json_extract(value,'$.detail') AS BLOB))>=384 "
        "OR json_type(value,'$.mono_ms') IS NOT 'integer' OR json_extract(value,'$.mono_ms')<0 "
        "OR json_type(value,'$.utc_ms') IS NOT 'integer' OR json_extract(value,'$.utc_ms') NOT "
        "BETWEEN 0 AND 253402300799999 "
        "OR json_type(value,'$.pod') IS NOT 'integer' OR json_extract(value,'$.pod') NOT BETWEEN 0 "
        "AND 127 "
        "OR json_type(value,'$.channel') IS NOT 'integer' OR json_extract(value,'$.channel') NOT "
        "BETWEEN -3 AND 1)) "
        "AND NOT EXISTS(SELECT 1 FROM json_tree(?1) WHERE key IS NOT NULL AND typeof(key)='text' "
        "GROUP BY parent,key HAVING count(*)>1) "
        "FROM spool_meta m,origin_runs r WHERE m.id=1 AND r.run_id=?3";
    int rc = prepare(o, sql, &s);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(s, 1, payload, (int)bytes, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(s, 2, order);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(s, 3, run, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(s, 4, batch);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(s, 5, first);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(s, 6, count);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(s, 7, readings);
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(s);
        if (rc == SQLITE_ROW)
            rc = sqlite3_column_int(s, 0) == 1 ? SQLITE_OK : SQLITE_CORRUPT;
        else if (rc == SQLITE_DONE)
            rc = SQLITE_CORRUPT;
    }
    rc = finish(s, rc);
    return rc == SQLITE_OK ? payload_metadata_valid(o, payload, bytes) : rc;
}
static int validate_schema(struct vigil_outbox *o)
{
    int rc =
        expect(o, "SELECT count(*) FROM sqlite_schema WHERE type IN ('table','trigger','view')", 5);
    for (size_t i = 0; rc == SQLITE_OK && i < sizeof(schema) / sizeof(schema[0]) - 1U; ++i) {
        const char *cursor = schema[i];
        bool trigger = strncmp(cursor, "CREATE TRIGGER ", 15) == 0;
        const char *name = cursor + (trigger ? 15 : 13);
        const char *end_name = strpbrk(name, "( ");
        const char *end = trigger ? strstr(cursor, " END;") : strchr(cursor, ';');
        if (end_name == NULL || end == NULL) return SQLITE_CORRUPT;
        if (trigger) end += 4;
        sqlite3_stmt *stmt = NULL;
        rc = prepare(o, "SELECT sql FROM sqlite_schema WHERE name=?1", &stmt);
        if (rc == SQLITE_OK)
            rc = sqlite3_bind_text(stmt, 1, name, (int)(end_name - name), SQLITE_STATIC);
        if (rc == SQLITE_OK) {
            rc = sqlite3_step(stmt);
            if (rc == SQLITE_ROW) {
                const char *got = (const char *)sqlite3_column_text(stmt, 0);
                size_t len = (size_t)(end - cursor);
                rc = got != NULL && strlen(got) == len && memcmp(got, cursor, len) == 0
                         ? SQLITE_OK
                         : SQLITE_CORRUPT;
            } else if (rc == SQLITE_DONE)
                rc = SQLITE_CORRUPT;
        }
        rc = finish(stmt, rc);
    }
    return rc;
}
static int load_usage(struct vigil_outbox *o)
{
    sqlite3_stmt *s = NULL;
    int rc =
        prepare(o,
                "SELECT "
                "spool_id,next_order_id,pending_records,pending_batches,pending_payload_bytes,"
                "admitted_records,admitted_readings,retired_records FROM spool_meta WHERE id=1",
                &s);
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(s);
        if (rc == SQLITE_ROW) {
            const char *spool = (const char *)sqlite3_column_text(s, 0);
            if (spool == NULL || !uuid(spool))
                rc = SQLITE_CORRUPT;
            else {
                memcpy(o->spool, spool, sizeof(o->spool));
                o->next_order = sqlite3_column_int64(s, 1);
                o->usage =
                    (struct vigil_outbox_usage){.records = (uint64_t)sqlite3_column_int64(s, 2),
                                                .batches = (uint64_t)sqlite3_column_int64(s, 3),
                                                .bytes = (uint64_t)sqlite3_column_int64(s, 4),
                                                .admitted = (uint64_t)sqlite3_column_int64(s, 5),
                                                .readings = (uint64_t)sqlite3_column_int64(s, 6),
                                                .retired = (uint64_t)sqlite3_column_int64(s, 7)};
                rc = sqlite3_step(s);
                if (rc == SQLITE_DONE)
                    rc = SQLITE_OK;
                else if (rc == SQLITE_ROW)
                    rc = SQLITE_CORRUPT;
            }
        } else if (rc == SQLITE_DONE)
            rc = SQLITE_CORRUPT;
    }
    return finish(s, rc);
}
static int load_run(struct vigil_outbox *o)
{
    sqlite3_stmt *s = NULL;
    int rc = prepare(o,
                     "SELECT "
                     "next_seq,next_batch_id,captured_reading_drops,captured_event_drops,captured_"
                     "revision FROM origin_runs WHERE run_id=?1",
                     &s);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(s, 1, o->run, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(s);
        if (rc == SQLITE_ROW) {
            o->next_seq = sqlite3_column_int64(s, 0);
            o->next_batch = sqlite3_column_int64(s, 1);
            o->captured_readings = (uint64_t)sqlite3_column_int64(s, 2);
            o->captured_events = (uint64_t)sqlite3_column_int64(s, 3);
            o->revision = (uint64_t)sqlite3_column_int64(s, 4);
            rc = SQLITE_OK;
        } else if (rc == SQLITE_DONE)
            rc = SQLITE_CORRUPT;
    }
    return finish(s, rc);
}
static int validate(struct vigil_outbox *o)
{
    o->validation_deadline = vigil_now_ms() + 25000U;
    sqlite3_progress_handler(o->db, 1000, deadline, o);
    int rc = validate_schema(o);
    if (rc == SQLITE_OK) rc = expect(o, "PRAGMA application_id", 1447642956);
    if (rc == SQLITE_OK) rc = expect(o, "PRAGMA user_version", 1);
    if (rc == SQLITE_OK)
        rc = expect(o, "SELECT count(*) FROM pragma_quick_check WHERE quick_check!='ok'", 0);
    if (rc == SQLITE_OK) rc = expect(o, "SELECT count(*) FROM pragma_foreign_key_check", 0);
    if (rc == SQLITE_OK)
        rc = expect(o,
                    "SELECT count(*) FROM pragma_table_list WHERE name IN "
                    "('spool_meta','origin_runs','outbox_batches') AND strict=1",
                    3);
    if (rc == SQLITE_OK)
        rc = expect(o,
                    "SELECT count(*) FROM sqlite_schema WHERE type='trigger' AND "
                    "name='immutable_batch' AND sql LIKE '%BEFORE UPDATE ON outbox_batches%'",
                    1);
    if (rc == SQLITE_OK) rc = expect(o, "SELECT count(*) FROM spool_meta", 1);
    /* Stage 1 has no retirement. Refuse to adopt a future/mutated retirement. */
    if (rc == SQLITE_OK)
        rc = expect(
            o,
            "SELECT count(*) FROM spool_meta WHERE last_retired_order=0 AND retired_records=0 AND "
            "retired_readings=0 "
            "AND pending_records=(SELECT coalesce(sum(record_count),0) FROM outbox_batches) "
            "AND pending_batches=(SELECT count(*) FROM outbox_batches) "
            "AND pending_payload_bytes=(SELECT coalesce(sum(payload_bytes),0) FROM outbox_batches) "
            "AND admitted_readings=(SELECT coalesce(sum(reading_count),0) FROM outbox_batches) "
            "AND next_order_id=pending_batches+1 AND next_run_no>(SELECT coalesce(max(run_no),0) "
            "FROM origin_runs)",
            1);
    if (rc == SQLITE_OK)
        rc = expect(
            o,
            "SELECT count(*) FROM origin_runs r WHERE next_seq!=admitted_records+1 OR "
            "next_batch_id!=(SELECT count(*)+1 FROM outbox_batches b WHERE b.run_id=r.run_id) OR "
            "admitted_records!=(SELECT coalesce(sum(record_count),0) FROM outbox_batches b WHERE "
            "b.run_id=r.run_id) OR "
            "admitted_readings!=(SELECT coalesce(sum(reading_count),0) FROM outbox_batches b WHERE "
            "b.run_id=r.run_id) OR "
            "retired_records!=0 OR retired_readings!=0 OR retired_revision!=0 OR "
            "retired_reading_drops!=0 OR retired_event_drops!=0",
            0);
    if (rc == SQLITE_OK) rc = expect(o, "SELECT count(*)<=4097 FROM origin_runs", 1);
    if (rc == SQLITE_OK) rc = load_usage(o);
    sqlite3_stmt *s = NULL;
    if (rc == SQLITE_OK) rc = prepare(o, "SELECT run_id,boot_id FROM origin_runs", &s);
    while (rc == SQLITE_OK) {
        int step = sqlite3_step(s);
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            rc = step;
            break;
        }
        const char *run = (const char *)sqlite3_column_text(s, 0),
                   *boot = (const char *)sqlite3_column_text(s, 1);
        if (run == NULL || boot == NULL || !uuid(run) || !uuid(boot)) rc = SQLITE_CORRUPT;
    }
    rc = finish(s, rc);
    s = NULL;
    if (rc == SQLITE_OK)
        rc = prepare(o,
                     "SELECT "
                     "order_id,run_id,batch_id,coalesce(first_seq,0),record_count,reading_count,"
                     "payload,payload_bytes FROM outbox_batches ORDER BY order_id",
                     &s);
    int64_t order = 1;
    while (rc == SQLITE_OK) {
        int step = sqlite3_step(s);
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            rc = step;
            break;
        }
        const char *run = (const char *)sqlite3_column_text(s, 1),
                   *payload = (const char *)sqlite3_column_text(s, 6);
        int bytes = sqlite3_column_bytes(s, 6);
        if (sqlite3_column_int64(s, 0) != order++ || run == NULL || payload == NULL || bytes <= 0 ||
            bytes != (int64_t)sqlite3_column_int64(s, 7) ||
            memchr(payload, 0, (size_t)bytes) != NULL) {
            rc = SQLITE_CORRUPT;
            break;
        }
        rc = payload_valid(o, payload, (size_t)bytes, sqlite3_column_int64(s, 0), run,
                           sqlite3_column_int64(s, 2), sqlite3_column_int64(s, 3),
                           sqlite3_column_int64(s, 4), sqlite3_column_int64(s, 5));
    }
    rc = finish(s, rc);
    if (rc == SQLITE_OK)
        rc = expect(o,
                    "SELECT count(*) FROM outbox_batches b WHERE batch_id!=(SELECT count(*) FROM "
                    "outbox_batches p WHERE p.run_id=b.run_id AND p.order_id<=b.order_id) "
                    "OR (record_count>0 AND first_seq!=(SELECT coalesce(sum(record_count),0)+1 "
                    "FROM outbox_batches p WHERE p.run_id=b.run_id AND p.order_id<b.order_id))",
                    0);
    if (rc == SQLITE_OK)
        rc =
            expect(o,
                   "SELECT count(*) FROM origin_runs r WHERE captured_reading_drops!=(SELECT "
                   "coalesce(sum(json_extract(payload,'$.reading_drops')),0) FROM outbox_batches b "
                   "WHERE b.run_id=r.run_id) "
                   "OR captured_event_drops!=(SELECT "
                   "coalesce(sum(json_extract(payload,'$.event_drops')),0) FROM outbox_batches b "
                   "WHERE b.run_id=r.run_id) "
                   "OR captured_revision!=coalesce((SELECT json_extract(payload,'$.revision') FROM "
                   "outbox_batches b WHERE b.run_id=r.run_id ORDER BY batch_id DESC LIMIT 1),0)",
                   0);
    sqlite3_progress_handler(o->db, 0, NULL, NULL);
    return rc;
}
static int mutation_ready(struct vigil_outbox *o)
{
    int rc = files_check(o);
    struct stat st;
    char name[NAME_MAX + 8];
    (void)snprintf(name, sizeof(name), "%s-wal", o->base);
    bool truncate = o->checkpoint_blocked;
    if (rc == SQLITE_OK && fstatat(o->dir, name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
        (uint64_t)st.st_size >= WAL_TRIGGER)
        truncate = true;
    if (rc == SQLITE_OK && truncate)
        rc = vigil_outbox_checkpoint(o, true) == VIGIL_STORE_OK ? SQLITE_OK : o->error;
    return rc;
}
static int seal(struct vigil_outbox *o)
{
    int rc = exec_sql(o, "COMMIT");
    if (rc == SQLITE_OK) {
        vigil_test_hook("after_sqlite_commit");
        rc = directory_sync(o);
    }
    return rc == SQLITE_OK ? rc : rollback(o, rc);
}
enum vigil_store_result vigil_outbox_open(struct vigil_outbox **out, const char *path,
                                          bool bootstrap)
{
    vigil_queue_assert_io();
    *out = NULL;
    if (path == NULL || path[0] != '/' || strlen(path) >= PATH_MAX ||
        sqlite3_libversion_number() < 3051003 || sqlite3_threadsafe() == 0)
        return VIGIL_STORE_INVALID;
    (void)sqlite3_hard_heap_limit64(32 * 1024 * 1024);
    if (sqlite3_hard_heap_limit64(-1) != 32 * 1024 * 1024) return VIGIL_STORE_INVALID;
    struct vigil_outbox *o = calloc(1, sizeof(*o));
    if (o == NULL) return VIGIL_STORE_NOMEM;
    o->owner = pthread_self();
    o->dir = -1;
    o->lock = -1;
    memcpy(o->path, path, strlen(path) + 1U);
    char parent[PATH_MAX];
    memcpy(parent, path, strlen(path) + 1U);
    char *slash = strrchr(parent, '/');
    int rc = SQLITE_CANTOPEN;
    if (slash == NULL || slash[1] == '\0' || strlen(slash + 1) > NAME_MAX - 8U) goto fail;
    memcpy(o->base, slash + 1, strlen(slash + 1) + 1U);
    *slash = '\0';
    char resolved[PATH_MAX];
    if (realpath(parent, resolved) == NULL || strcmp(parent, resolved) != 0) goto fail;
    o->dir = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    struct statfs fs;
    if (o->dir < 0 || fstat(o->dir, &st) != 0 || st.st_uid != geteuid() ||
        (st.st_mode & 0777) != 0700 || fstatfs(o->dir, &fs) != 0 ||
        (fs.f_type != 0xef53 && fs.f_type != 0x58465342))
        goto fail;
    o->lock = openat(o->dir, "outbox.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (o->lock < 0 || fstat(o->lock, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
        st.st_uid != geteuid() || (st.st_mode & 0777) != 0600)
        goto fail;
    if (flock(o->lock, LOCK_EX | LOCK_NB) != 0) {
        rc = SQLITE_BUSY;
        goto fail;
    }
    if (bootstrap) {
        struct statvfs space;
        if (fstatvfs(o->dir, &space) != 0 || (uint64_t)space.f_bavail * (uint64_t)space.f_frsize <
                                                 UINT64_C(2) * 1024U * 1024U * 1024U)
            goto fail;
        DIR *d = fdopendir(dup(o->dir));
        if (d == NULL) goto fail;
        bool empty = true;
        errno = 0;
        struct dirent *entry;
        while ((entry = readdir(d)) != NULL)
            if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0 &&
                strcmp(entry->d_name, "outbox.lock") != 0)
                empty = false;
        if (errno != 0) empty = false;
        if (closedir(d) != 0) empty = false;
        if (!empty) goto fail;
        int fd = openat(o->dir, o->base, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) goto fail;
        if (close(fd) != 0) goto fail;
    } else if ((rc = files_check(o)) != SQLITE_OK)
        goto fail;
    rc = connect_store(o, bootstrap);
    if (rc != SQLITE_OK) goto fail;
    if (bootstrap) {
        if (!random_hex(o->spool, 16, true)) {
            rc = SQLITE_IOERR;
            goto fail;
        }
        rc = exec_sql(o, "BEGIN IMMEDIATE");
        for (size_t i = 0; rc == SQLITE_OK && i < sizeof(schema) / sizeof(schema[0]); ++i)
            rc = exec_sql(o, schema[i]);
        sqlite3_stmt *s = NULL;
        if (rc == SQLITE_OK)
            rc = prepare(o, "INSERT INTO spool_meta VALUES(1,?1,1,1,0,0,0,0,0,0,0,0,0)", &s);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(s, 1, o->spool, -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) {
            rc = sqlite3_step(s);
            if (rc == SQLITE_DONE) rc = SQLITE_OK;
        }
        rc = finish(s, rc);
        if (rc == SQLITE_OK) rc = seal(o);
    }
    if (rc == SQLITE_OK) rc = validate(o);
    if (rc != SQLITE_OK) goto fail;
    *out = o;
    return VIGIL_STORE_OK;
fail:
    if (o->db != NULL) {
        rc = rollback(o, rc);
        (void)sqlite3_close(o->db);
    }
    if (o->lock >= 0) (void)close(o->lock);
    if (o->dir >= 0) (void)close(o->dir);
    free(o);
    switch (rc & 255) {
    case SQLITE_BUSY:
        return VIGIL_STORE_BUSY;
    case SQLITE_CORRUPT:
    case SQLITE_NOTADB:
        return VIGIL_STORE_CORRUPT;
    case SQLITE_NOMEM:
        return VIGIL_STORE_NOMEM;
    default:
        return VIGIL_STORE_INVALID;
    }
}
enum vigil_store_result vigil_outbox_begin_run(struct vigil_outbox *o)
{
    owner(o);
    if (o->run[0] != '\0') return VIGIL_STORE_INVALID;
    uint64_t now = vigil_now_ms(), utc = vigil_utc_ms();
    if (now > (uint64_t)ID_MAX || utc > (uint64_t)UTC_MAX) return VIGIL_STORE_INVALID;
    o->started = (int64_t)now;
    o->utc = (int64_t)utc;
    int fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return VIGIL_STORE_INVALID;
    ssize_t got = read(fd, o->boot, 36);
    int closed = close(fd);
    if (got != 36 || closed != 0 || !uuid(o->boot)) return VIGIL_STORE_INVALID;
    int rc = mutation_ready(o);
    if (rc == SQLITE_OK) rc = exec_sql(o, "BEGIN IMMEDIATE");
    if (rc == SQLITE_OK)
        rc = scalar(o, "SELECT next_run_no FROM spool_meta WHERE id=1", &o->run_no);
    if (rc == SQLITE_OK && o->run_no > ID_MAX) {
        (void)rollback(o, SQLITE_FULL);
        return VIGIL_STORE_EXHAUSTED;
    }
    for (unsigned attempt = 0; rc == SQLITE_OK && attempt < 3U; ++attempt) {
        if (!random_hex(o->run, 16, true) || !random_hex(o->token, 32, false)) {
            rc = SQLITE_IOERR;
            break;
        }
        sqlite3_stmt *s = NULL;
        rc = prepare(o, "INSERT INTO origin_runs VALUES(?1,?2,?3,?4,?5,?6,1,1,0,0,0,0,0,0,0,0,0,0)",
                     &s);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(s, 1, o->run, -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(s, 2, o->run_no);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(s, 3, o->token, -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(s, 4, o->boot, -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(s, 5, o->utc);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(s, 6, o->started);
        if (rc == SQLITE_OK) {
            rc = sqlite3_step(s);
            if (rc == SQLITE_DONE) rc = SQLITE_OK;
        }
        rc = finish(s, rc);
        if (rc == SQLITE_OK) break;
        if ((rc == SQLITE_CONSTRAINT_PRIMARYKEY || rc == SQLITE_CONSTRAINT_UNIQUE) && attempt < 2U)
            rc = SQLITE_OK;
    }
    if (rc == SQLITE_OK)
        rc = exec_sql(o, "UPDATE spool_meta SET next_run_no=next_run_no+1 WHERE id=1;DELETE FROM "
                         "origin_runs WHERE admitted_records=0 AND next_batch_id=1 AND "
                         "run_no<(SELECT max(run_no) FROM origin_runs)");
    if (rc == SQLITE_OK) rc = seal(o);
    if (rc != SQLITE_OK) return result(o, rollback(o, rc));
    vigil_test_hook("after_startup_origin_commit");
    o->next_seq = 1;
    o->next_batch = 1;
    return result(o, SQLITE_OK);
}
static void json_add(struct vigil_outbox *o, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void json_add(struct vigil_outbox *o, const char *fmt, ...)
{
    if (!o->json_ok) return;
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(o->payload + o->used, sizeof(o->payload) - o->used, fmt, args);
    va_end(args);
    if (n < 0 || (size_t)n >= sizeof(o->payload) - o->used) {
        o->json_ok = false;
        return;
    }
    o->used += (size_t)n;
}
static bool utf8(const unsigned char *s)
{
    while (*s != 0) {
        uint32_t v;
        unsigned n;
        if (*s < 128U) {
            ++s;
            continue;
        }
        if (*s >= 0xc2U && *s <= 0xdfU) {
            v = *s & 31U;
            n = 1;
        } else if (*s >= 0xe0U && *s <= 0xefU) {
            v = *s & 15U;
            n = 2;
        } else if (*s >= 0xf0U && *s <= 0xf4U) {
            v = *s & 7U;
            n = 3;
        } else
            return false;
        unsigned count = n;
        ++s;
        while (n-- != 0) {
            if ((*s & 0xc0U) != 0x80U) return false;
            v = (v << 6) | (*s++ & 63U);
        }
        if ((count == 2U && v < 0x800U) || (count == 3U && v < 0x10000U) || v > 0x10ffffU ||
            (v >= 0xd800U && v <= 0xdfffU))
            return false;
    }
    return true;
}
static bool valid_record(const struct vigil_record *r, bool alarm)
{
    if (memchr(r->kind, 0, sizeof(r->kind)) == NULL ||
        memchr(r->detail, 0, sizeof(r->detail)) == NULL || r->mono_ms > (uint64_t)ID_MAX ||
        r->utc_ms > (uint64_t)UTC_MAX || r->pod > 127U || r->channel < -3 || r->channel > 1 ||
        !utf8((const unsigned char *)r->detail))
        return false;
    bool reading = strcmp(r->kind, "reading") == 0;
    if (r->reading != reading || (alarm && strcmp(r->kind, "alarm") != 0)) return false;
    return reading || strcmp(r->kind, "heartbeat") == 0 || strcmp(r->kind, "rejected") == 0 ||
           strcmp(r->kind, "alarm") == 0;
}
static void record_json(struct vigil_outbox *o, const struct vigil_record *r, uint64_t seq,
                        bool comma)
{
    json_add(o,
             "%s{\"seq\":%" PRIu64 ",\"mono_ms\":%" PRIu64 ",\"utc_ms\":%" PRIu64
             ",\"pod\":%u,\"channel\":%d,\"kind\":\"%s\",\"detail\":%s}",
             comma ? "," : "", seq, r->mono_ms, r->utc_ms, r->pod, r->channel, r->kind, r->detail);
}
static bool make_payload(struct vigil_outbox *o, const struct vigil_capture *c)
{
    if (c->count > VIGIL_BATCH_SIZE || c->alarm_count == 0 ||
        c->alarm_count > VIGIL_ALARM_CAPACITY || c->revision > (uint64_t)ID_MAX ||
        c->ticket > (uint64_t)ID_MAX || c->stats.readings_dropped > (uint64_t)ID_MAX ||
        c->stats.events_dropped > (uint64_t)ID_MAX || c->stats.readings_queued > (uint64_t)ID_MAX ||
        c->stats.events_queued > (uint64_t)ID_MAX ||
        c->stats.readings_dropped < o->captured_readings ||
        c->stats.events_dropped < o->captured_events || c->revision < o->revision)
        return false;
    for (size_t i = 0; i < c->count; ++i)
        if (!valid_record(&c->records[i], false) ||
            (i != 0 && c->records[i].seq <= c->records[i - 1U].seq))
            return false;
    for (size_t i = 0; i < c->alarm_count; ++i)
        if (!valid_record(&c->alarms[i], true)) return false;
    uint64_t now = vigil_now_ms(), utc = vigil_utc_ms();
    if (now > (uint64_t)ID_MAX || utc > (uint64_t)UTC_MAX) return false;
    uint64_t rd = c->stats.readings_dropped - o->captured_readings,
             ed = c->stats.events_dropped - o->captured_events;
    bool snapshot = o->next_batch == 1 || rd != 0 || ed != 0 || c->revision != o->revision;
    o->used = 0;
    o->json_ok = true;
    json_add(o,
             "{\"format\":2,\"order_id\":%" PRId64 ",\"run_id\":\"%s\",\"batch_id\":%" PRId64
             ",\"started_ms\":%" PRId64 ",\"started_utc_ms\":%" PRId64 ",\"mono_ms\":%" PRIu64
             ",\"utc_ms\":%" PRIu64
             ",\"origin\":{\"spool_id\":\"%s\",\"run_id\":\"%s\",\"run_no\":%" PRId64
             ",\"run_token\":\"%s\",\"boot_id\":\"%s\",\"started_ms\":%" PRId64
             ",\"started_utc_ms\":%" PRId64 "}"
             ",\"capture_ticket\":%" PRIu64 ",\"revision\":%" PRIu64
             ",\"kind\":\"%s\",\"reading_drops\":%" PRIu64 ",\"event_drops\":%" PRIu64
             ",\"local_loss\":{\"reading_evicted\":%" PRIu64 ",\"event_evicted\":%" PRIu64
             ",\"reading_rejected\":%" PRIu64 ",\"event_rejected\":%" PRIu64
             "},\"health\":{\"readings_queued\":%" PRIu64 ",\"events_queued\":%" PRIu64
             ",\"readings_dropped\":%" PRIu64 ",\"events_dropped\":%" PRIu64
             ",\"first_drop_ms\":%" PRIu64 ",\"last_drop_ms\":%" PRIu64 ",\"revision\":%" PRIu64
             ",\"outage_started_ms\":0,\"outage_ms\":0,\"outage_total_ms\":0,\"outage_ended_ms\":0,"
             "\"connection_failures\":0,\"query_failures\":0,\"reconnects\":0},\"records\":[",
             o->next_order, o->run, o->next_batch, o->started, o->utc, now, utc, o->spool, o->run,
             o->run_no, o->token, o->boot, o->started, o->utc, c->ticket, c->revision,
             rd != 0 || ed != 0   ? "gap"
             : o->next_batch == 1 ? "recovery"
                                  : "flush",
             rd, ed, c->stats.readings_evicted, c->stats.events_evicted, c->stats.readings_rejected,
             c->stats.events_rejected, c->stats.readings_queued, c->stats.events_queued,
             c->stats.readings_dropped, c->stats.events_dropped, c->stats.first_drop_ms,
             c->stats.last_drop_ms, c->revision);
    uint64_t readings = 0;
    for (size_t i = 0; i < c->count; ++i) {
        record_json(o, &c->records[i], (uint64_t)o->next_seq + i, i != 0);
        if (c->records[i].reading) ++readings;
    }
    json_add(o, "],\"alarms\":");
    if (snapshot) {
        json_add(o, "[");
        for (size_t i = 0; i < c->alarm_count; ++i)
            record_json(o, &c->alarms[i], 0, i != 0);
        json_add(o, "]}");
    } else
        json_add(o, "null}");
    o->pending_reading_drops = c->stats.readings_dropped;
    o->pending_event_drops = c->stats.events_dropped;
    o->pending_revision = c->revision;
    o->candidate =
        (struct vigil_local_commit){.order_id = (uint64_t)o->next_order,
                                    .batch_id = (uint64_t)o->next_batch,
                                    .first_seq = c->count == 0 ? 0 : (uint64_t)o->next_seq,
                                    .records = c->count,
                                    .readings = readings};
    return o->json_ok;
}
static int recover(struct vigil_outbox *o, bool *found)
{
    *found = false;
    o->recovering = true;
    if (o->db != NULL) {
        int rc = sqlite3_close(o->db);
        if (rc != SQLITE_OK) {
            o->recovering = false;
            return rc;
        }
        o->db = NULL;
    }
    int rc = files_check(o);
    if (rc == SQLITE_OK) rc = connect_store(o, false);
    if (rc == SQLITE_OK) rc = validate(o);
    if (rc == SQLITE_OK) rc = load_run(o);
    sqlite3_stmt *s = NULL;
    if (rc == SQLITE_OK && o->pending)
        rc = prepare(o, "SELECT run_id,batch_id,payload FROM outbox_batches WHERE order_id=?1", &s);
    if (rc == SQLITE_OK && o->pending)
        rc = sqlite3_bind_int64(s, 1, (int64_t)o->candidate.order_id);
    if (rc == SQLITE_OK && o->pending) {
        rc = sqlite3_step(s);
        if (rc == SQLITE_ROW) {
            const char *run = (const char *)sqlite3_column_text(s, 0),
                       *payload = (const char *)sqlite3_column_text(s, 2);
            if (run == NULL || payload == NULL || strcmp(run, o->run) != 0 ||
                sqlite3_column_int64(s, 1) != (int64_t)o->candidate.batch_id ||
                sqlite3_column_bytes(s, 2) != (int)o->used ||
                memcmp(payload, o->payload, o->used) != 0)
                rc = SQLITE_CORRUPT;
            else {
                *found = true;
                rc = SQLITE_OK;
            }
        } else if (rc == SQLITE_DONE) {
            rc = (o->next_order == (int64_t)o->candidate.order_id &&
                  o->next_batch == (int64_t)o->candidate.batch_id &&
                  (o->candidate.records == 0 || o->next_seq == (int64_t)o->candidate.first_seq))
                     ? SQLITE_OK
                     : SQLITE_CORRUPT;
        }
    }
    rc = finish(s, rc);
    if (rc == SQLITE_OK) rc = mutation_ready(o);
    if (rc == SQLITE_OK) rc = exec_sql(o, "BEGIN IMMEDIATE");
    if (rc == SQLITE_OK)
        rc = exec_sql(o, "UPDATE spool_meta SET recovery_generation=recovery_generation+1 WHERE "
                         "id=1 AND recovery_generation<9223372036854775806");
    if (rc == SQLITE_OK && sqlite3_changes(o->db) != 1) rc = SQLITE_FULL;
    if (rc == SQLITE_OK) rc = seal(o);
    if (rc == SQLITE_OK) o->uncertain = false;
    o->recovering = false;
    return rc;
}
enum vigil_store_result vigil_outbox_append(struct vigil_outbox *o, const struct vigil_capture *c,
                                            struct vigil_local_commit *committed)
{
    owner(o);
    if (o->run[0] == '\0') return VIGIL_STORE_INVALID;
    int rc = SQLITE_OK;
    bool found = false;
    if (o->uncertain) {
        rc = recover(o, &found);
        if (rc != SQLITE_OK) return result(o, rc);
        if (found) {
            *committed = o->candidate;
            o->pending = false;
            return result(o, SQLITE_OK);
        }
    }
    if (o->next_seq > ID_MAX || o->next_batch > ID_MAX || o->next_order > ID_MAX ||
        c->count > (uint64_t)(INT64_MAX - o->next_seq))
        return VIGIL_STORE_EXHAUSTED;
    if (!o->pending) {
        if (!make_payload(o, c)) return VIGIL_STORE_INVALID;
        o->pending = true;
    }
    if (o->usage.records > vigil_test_limit("VIGIL_TEST_RECORD_LIMIT", VIGIL_OUTBOX_RECORDS) ||
        o->candidate.records >
            vigil_test_limit("VIGIL_TEST_RECORD_LIMIT", VIGIL_OUTBOX_RECORDS) - o->usage.records ||
        o->usage.batches >= vigil_test_limit("VIGIL_TEST_BATCH_LIMIT", VIGIL_OUTBOX_BATCHES) ||
        o->usage.bytes > vigil_test_limit("VIGIL_TEST_BYTE_LIMIT", VIGIL_OUTBOX_BYTES) ||
        o->used > vigil_test_limit("VIGIL_TEST_BYTE_LIMIT", VIGIL_OUTBOX_BYTES) - o->usage.bytes)
        return result(o, SQLITE_FULL);
    if (vigil_test_fault("read_io")) (void)sqlite3_db_release_memory(o->db);
    rc = payload_valid(o, o->payload, o->used, (int64_t)o->candidate.order_id, o->run,
                       (int64_t)o->candidate.batch_id, (int64_t)o->candidate.first_seq,
                       (int64_t)o->candidate.records, (int64_t)o->candidate.readings);
    if (rc != SQLITE_OK) {
        if ((rc & 255) == SQLITE_IOERR || (rc & 255) == SQLITE_CANTOPEN ||
            (rc & 255) == SQLITE_PROTOCOL)
            o->uncertain = true;
        if (rc == SQLITE_CORRUPT || rc == SQLITE_ERROR) {
            o->pending = false;
            return result(o, SQLITE_MISMATCH);
        }
        return result(o, rc);
    }
    rc = mutation_ready(o);
    vigil_test_hook("after_take_before_begin");
    if (rc == SQLITE_OK) rc = exec_sql(o, "BEGIN IMMEDIATE");
    if (rc == SQLITE_OK) vigil_test_hook("after_local_begin");
    sqlite3_stmt *s = NULL;
    if (rc == SQLITE_OK)
        rc = prepare(o, "INSERT INTO outbox_batches VALUES(?1,?2,?3,?4,?5,?6,?7,?8)", &s);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(s, 1, (int64_t)o->candidate.order_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(s, 2, o->run, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(s, 3, (int64_t)o->candidate.batch_id);
    if (rc == SQLITE_OK)
        rc = o->candidate.records == 0 ? sqlite3_bind_null(s, 4)
                                       : sqlite3_bind_int64(s, 4, (int64_t)o->candidate.first_seq);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(s, 5, (int64_t)o->candidate.records);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(s, 6, (int64_t)o->candidate.readings);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(s, 7, o->payload, (int)o->used, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(s, 8, (int64_t)o->used);
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(s);
        if (rc == SQLITE_DONE) rc = SQLITE_OK;
    }
    rc = finish(s, rc);
    s = NULL;
    if (rc == SQLITE_OK)
        rc = prepare(o,
                     "UPDATE origin_runs SET next_seq=next_seq+?1,next_batch_id=next_batch_id+1,"
                     "admitted_records=admitted_records+?1,admitted_readings=admitted_readings+?2,"
                     "captured_reading_drops=?3,captured_event_drops=?4,captured_revision=?5 "
                     "WHERE run_id=?6 AND next_batch_id=?7 AND next_seq=?8",
                     &s);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(s, 1, (int64_t)o->candidate.records);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(s, 2, (int64_t)o->candidate.readings);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(s, 3, (int64_t)o->pending_reading_drops);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(s, 4, (int64_t)o->pending_event_drops);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(s, 5, (int64_t)o->pending_revision);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(s, 6, o->run, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(s, 7, (int64_t)o->candidate.batch_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(s, 8, o->next_seq);
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(s);
        if (rc == SQLITE_DONE) rc = sqlite3_changes(o->db) == 1 ? SQLITE_OK : SQLITE_CORRUPT;
    }
    rc = finish(s, rc);
    s = NULL;
    if (rc == SQLITE_OK)
        rc = prepare(o,
                     "UPDATE spool_meta SET "
                     "next_order_id=next_order_id+1,pending_records=pending_records+?1,"
                     "pending_batches=pending_batches+1,pending_payload_bytes=pending_payload_"
                     "bytes+?2,admitted_records=admitted_records+?1,"
                     "admitted_readings=admitted_readings+?3 WHERE id=1 AND next_order_id=?4",
                     &s);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(s, 1, (int64_t)o->candidate.records);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(s, 2, (int64_t)o->used);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(s, 3, (int64_t)o->candidate.readings);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(s, 4, (int64_t)o->candidate.order_id);
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(s);
        if (rc == SQLITE_DONE) rc = sqlite3_changes(o->db) == 1 ? SQLITE_OK : SQLITE_CORRUPT;
    }
    rc = finish(s, rc);
    if (rc == SQLITE_OK) {
        vigil_test_hook("after_local_insert_before_commit");
        if (vigil_test_fault("rollback")) rc = SQLITE_FULL;
    }
    if (rc == SQLITE_OK)
        rc = seal(o);
    else
        rc = rollback(o, rc);
    if (rc == SQLITE_OK) {
        vigil_test_hook("after_local_commit_before_publish");
        rc = load_usage(o);
        if (rc == SQLITE_OK) rc = load_run(o);
        if (rc != SQLITE_OK) o->uncertain = true;
    }
    if ((rc & 255) == SQLITE_IOERR || (rc & 255) == SQLITE_CANTOPEN ||
        (rc & 255) == SQLITE_PROTOCOL)
        o->uncertain = true;
    if (rc != SQLITE_OK) return result(o, rc);
    *committed = o->candidate;
    o->pending = false;
    return result(o, SQLITE_OK);
}
enum vigil_store_result vigil_outbox_checkpoint(struct vigil_outbox *o, bool truncate)
{
    owner(o);
    if (o->uncertain && !o->recovering) {
        bool found = false;
        int recovered = recover(o, &found);
        if (recovered != SQLITE_OK) return result(o, recovered);
        /* A pending admission may have committed. Only append may publish
         * it; keep the uncertainty marker until append reconciles its copy. */
        if (o->pending) o->uncertain = true;
    }
    vigil_test_hook("before_wal_checkpoint");
    int log = -1, done = -1;
    int rc = sqlite3_wal_checkpoint_v2(
        o->db, "main", truncate ? SQLITE_CHECKPOINT_TRUNCATE : SQLITE_CHECKPOINT_PASSIVE, &log,
        &done);
    if (rc == SQLITE_OK && (log < 0 || done != log)) rc = SQLITE_BUSY;
    if (rc == SQLITE_OK) rc = directory_sync(o);
    vigil_test_hook("after_wal_checkpoint");
    o->checkpoint_blocked = rc != SQLITE_OK;
    if ((rc & 255) == SQLITE_IOERR || (rc & 255) == SQLITE_CANTOPEN ||
        (rc & 255) == SQLITE_PROTOCOL)
        o->uncertain = true;
    return result(o, rc);
}
enum vigil_store_result vigil_outbox_close(struct vigil_outbox *o)
{
    if (o == NULL) return VIGIL_STORE_OK;
    owner(o);
    int rc = SQLITE_OK;
    if (o->db != NULL) {
        o->recovering = true;
        enum vigil_store_result checkpoint = vigil_outbox_checkpoint(o, true);
        if (checkpoint != VIGIL_STORE_OK) rc = o->error;
        int end = sqlite3_close(o->db);
        if (rc == SQLITE_OK) rc = end;
    }
    if (close(o->lock) != 0) rc = SQLITE_IOERR_CLOSE;
    if (close(o->dir) != 0) rc = SQLITE_IOERR_CLOSE;
    enum vigil_store_result r = result(o, rc);
    free(o);
    return r;
}
void vigil_outbox_usage(const struct vigil_outbox *o, struct vigil_outbox_usage *usage)
{
    *usage = o->usage;
}
const char *vigil_outbox_run_id(const struct vigil_outbox *o)
{
    return o->run;
}
int vigil_outbox_error(const struct vigil_outbox *o)
{
    return o->error;
}
const char *vigil_store_name(enum vigil_store_result r)
{
    static const char *const names[] = {"ready",   "full",  "busy",      "recovering", "fault",
                                        "invalid", "nomem", "exhausted", "readonly"};
    return (unsigned)r < sizeof(names) / sizeof(names[0]) ? names[r] : "invalid";
}
