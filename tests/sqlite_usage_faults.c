/* Linker wrappers in test targets only. Inject SQLite's NOMEM result at the
 * usage query API boundaries; all transactions/files remain real SQLite. */
#include "vigil/failpoint.h"
#include <sqlite3.h>
#include <stdbool.h>
#include <string.h>

int __real_sqlite3_prepare_v2(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
int __wrap_sqlite3_prepare_v2(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
int __real_sqlite3_step(sqlite3_stmt *);
int __wrap_sqlite3_step(sqlite3_stmt *);

static bool usage(const char *sql)
{
    return sql != NULL && strstr(sql, "spool_id,next_order_id,pending_records") != NULL;
}
int __wrap_sqlite3_prepare_v2(sqlite3 *db, const char *sql, int size,
                            sqlite3_stmt **stmt, const char **tail)
{
    if (usage(sql) && vigil_test_fault("usage_prepare_nomem")) {
        *stmt = NULL;
        return SQLITE_NOMEM;
    }
    return __real_sqlite3_prepare_v2(db, sql, size, stmt, tail);
}
int __wrap_sqlite3_step(sqlite3_stmt *stmt)
{
    if (usage(sqlite3_sql(stmt)) &&
        vigil_test_fault(sqlite3_data_count(stmt) == 0 ? "usage_step_nomem" : "usage_done_nomem"))
        return SQLITE_NOMEM;
    return __real_sqlite3_step(stmt);
}
