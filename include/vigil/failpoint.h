#ifndef VIGIL_FAILPOINT_H
#define VIGIL_FAILPOINT_H
#ifdef VIGIL_TEST_FAILPOINTS
/* Test binary only: exact-name/hit handshake, fixed preopened pipe FDs. */
void vigil_test_hook(const char *name);
int vigil_test_fault(const char *name);
unsigned long long vigil_test_limit(const char *name, unsigned long long fallback);
void vigil_test_vfs_install(void);
#else
#define vigil_test_hook(name) ((void)0)
#define vigil_test_fault(name) (0)
#define vigil_test_limit(name, fallback) (fallback)
#define vigil_test_vfs_install() ((void)0)
#endif
#endif
