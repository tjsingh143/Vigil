/* Transparent test-only SQLite VFS. Only xWrite/xSync/xLock fault returns differ
 * from the real Linux VFS. Actual file bytes, locks and barriers remain SQLite's. */
#include "vigil/failpoint.h"
#include <sqlite3.h>
#include <stddef.h>
#include <string.h>
struct wrapped {
    sqlite3_file base;
    sqlite3_file *real;
};
static sqlite3_vfs vfs;
static sqlite3_vfs *original;
static sqlite3_file *real(sqlite3_file *f)
{
    return ((struct wrapped *)f)->real;
}
static int xclose(sqlite3_file *f)
{
    return real(f)->pMethods->xClose(real(f));
}
static int xread(sqlite3_file *f, void *b, int n, sqlite3_int64 off)
{
    if (vigil_test_fault("read_io")) return SQLITE_IOERR_READ;
    return real(f)->pMethods->xRead(real(f), b, n, off);
}
static int xwrite(sqlite3_file *f, const void *b, int n, sqlite3_int64 off)
{
    if (vigil_test_fault("write_full")) return SQLITE_FULL;
    if (vigil_test_fault("write_io")) return SQLITE_IOERR_WRITE;
    if (vigil_test_fault("write_readonly")) return SQLITE_READONLY;
    return real(f)->pMethods->xWrite(real(f), b, n, off);
}
static int xtruncate(sqlite3_file *f, sqlite3_int64 n)
{
    return real(f)->pMethods->xTruncate(real(f), n);
}
static int xsync(sqlite3_file *f, int flags)
{
    if (vigil_test_fault("sync_io")) return SQLITE_IOERR_FSYNC;
    return real(f)->pMethods->xSync(real(f), flags);
}
static int xsize(sqlite3_file *f, sqlite3_int64 *n)
{
    return real(f)->pMethods->xFileSize(real(f), n);
}
static int xlock(sqlite3_file *f, int level)
{
    if (vigil_test_fault("lock_busy") && level >= SQLITE_LOCK_RESERVED) return SQLITE_BUSY;
    return real(f)->pMethods->xLock(real(f), level);
}
static int xunlock(sqlite3_file *f, int level)
{
    return real(f)->pMethods->xUnlock(real(f), level);
}
static int xreserved(sqlite3_file *f, int *out)
{
    return real(f)->pMethods->xCheckReservedLock(real(f), out);
}
static int xcontrol(sqlite3_file *f, int op, void *arg)
{
    return real(f)->pMethods->xFileControl(real(f), op, arg);
}
static int xsector(sqlite3_file *f)
{
    return real(f)->pMethods->xSectorSize(real(f));
}
static int xdevice(sqlite3_file *f)
{
    return real(f)->pMethods->xDeviceCharacteristics(real(f));
}
static int xmap(sqlite3_file *f, int page, int n, int extend, void volatile **out)
{
    return real(f)->pMethods->xShmMap(real(f), page, n, extend, out);
}
static int xshmlock(sqlite3_file *f, int off, int n, int flags)
{
    if (vigil_test_fault("lock_busy") && off == 0 && (flags & SQLITE_SHM_EXCLUSIVE) != 0 &&
        (flags & SQLITE_SHM_LOCK) != 0)
        return SQLITE_BUSY;
    /* SQLite 3.51.3 WAL checkpoint lock is slot 1. The writer lock at slot
     * 0 remains available, exposing accidental writes after checkpoint failure. */
    if (vigil_test_fault("checkpoint_busy") && off == 1 &&
        (flags & SQLITE_SHM_EXCLUSIVE) != 0 && (flags & SQLITE_SHM_LOCK) != 0)
        return SQLITE_BUSY;
    return real(f)->pMethods->xShmLock(real(f), off, n, flags);
}
static void xbarrier(sqlite3_file *f)
{
    real(f)->pMethods->xShmBarrier(real(f));
}
static int xunmap(sqlite3_file *f, int del)
{
    return real(f)->pMethods->xShmUnmap(real(f), del);
}
static int xfetch(sqlite3_file *f, sqlite3_int64 off, int n, void **out)
{
    return real(f)->pMethods->xFetch(real(f), off, n, out);
}
static int xunfetch(sqlite3_file *f, sqlite3_int64 off, void *p)
{
    return real(f)->pMethods->xUnfetch(real(f), off, p);
}
static const sqlite3_io_methods methods = {
    3,        xclose,  xread,   xwrite, xtruncate, xsync,    xsize,  xlock,  xunlock, xreserved,
    xcontrol, xsector, xdevice, xmap,   xshmlock,  xbarrier, xunmap, xfetch, xunfetch};
static int xopen(sqlite3_vfs *v, const char *name, sqlite3_file *file, int flags, int *out)
{
    (void)v;
    struct wrapped *w = (struct wrapped *)file;
    memset(w, 0, sizeof(*w));
    w->real = (sqlite3_file *)(void *)(w + 1);
    int rc = original->xOpen(original, name, w->real, flags, out);
    if (rc == SQLITE_OK) w->base.pMethods = &methods;
    return rc;
}
void vigil_test_vfs_install(void)
{
    if (original != NULL) return; /* every test process uses one owner */
    original = sqlite3_vfs_find(NULL);
    vfs = *original;
    vfs.zName = "vigil-test-vfs";
    vfs.szOsFile += (int)sizeof(struct wrapped);
    vfs.xOpen = xopen;
    if (sqlite3_vfs_register(&vfs, 1) != SQLITE_OK) original = NULL;
}
