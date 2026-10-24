#include "vigil/failpoint.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static _Thread_local char injected;

void vigil_test_hook(const char *name)
{
    static _Thread_local unsigned hits;
    static _Thread_local bool disabled;
    if (disabled) return;
    const char *selected = getenv("VIGIL_TEST_HOOK");
    if (selected == NULL || strcmp(selected, name) != 0) return;
    const char *nth = getenv("VIGIL_TEST_HIT");
    unsigned target = nth == NULL ? 1U : (unsigned)strtoul(nth, NULL, 10);
    const char *repeat_value = getenv("VIGIL_TEST_REPEAT");
    unsigned repeat = repeat_value == NULL ? 1U : (unsigned)strtoul(repeat_value, NULL, 10);
    ++hits;
    if (hits < target || hits - target >= repeat) return;
    const char *notify = getenv("VIGIL_TEST_NOTIFY_FD"), *release = getenv("VIGIL_TEST_RELEASE_FD");
    if (notify == NULL || release == NULL) abort();
    char token[80] = {0};
    (void)snprintf(token, sizeof(token), "%s:%u", name, hits);
    if (write(atoi(notify), token, sizeof(token)) != (ssize_t)sizeof(token)) abort();
    char byte;
    ssize_t n;
    do {
        n = read(atoi(release), &byte, 1);
    } while (n < 0 && errno == EINTR);
    if (n != 1) abort();
    injected = byte;
    /* Release the selected worker and disable further pauses in this thread.
     * Test fixture only; production does not link this dispatcher. */
    disabled = byte == 'D';
}
int vigil_test_fault(const char *name)
{
    const char *selected = getenv("VIGIL_TEST_FAULT");
    return (selected != NULL && strcmp(selected, name) == 0) ||
           (injected == 'F' && strcmp(name, "write_full") == 0) ||
           (injected == 'S' && strcmp(name, "sync_io") == 0) ||
           (injected == 'I' && strcmp(name, "write_io") == 0) ||
           (injected == 'R' && strcmp(name, "write_readonly") == 0) ||
           (injected == 'B' && strcmp(name, "lock_busy") == 0);
}

unsigned long long vigil_test_limit(const char *name, unsigned long long fallback)
{
    const char *s = getenv(name);
    return s == NULL ? fallback : strtoull(s, NULL, 10);
}
