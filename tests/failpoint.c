#include "vigil/failpoint.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static _Thread_local char injected;

static const char *setting(const char *key, unsigned index)
{
    if (index == 0) return getenv(key);
    char indexed[64];
    (void)snprintf(indexed, sizeof(indexed), "%s_%u", key, index);
    return getenv(indexed);
}
static void hook(const char *name, unsigned index)
{
    static _Thread_local unsigned hits[9];
    static _Thread_local bool disabled[9];
    if (disabled[index]) return;
    const char *selected = setting("VIGIL_TEST_HOOK", index);
    if (selected == NULL || strcmp(selected, name) != 0) return;
    const char *nth = setting("VIGIL_TEST_HIT", index);
    unsigned target = nth == NULL ? 1U : (unsigned)strtoul(nth, NULL, 10);
    const char *repeat_value = setting("VIGIL_TEST_REPEAT", index);
    unsigned repeat = repeat_value == NULL ? 1U : (unsigned)strtoul(repeat_value, NULL, 10);
    ++hits[index];
    if (hits[index] < target || hits[index] - target >= repeat) return;
    const char *notify = setting("VIGIL_TEST_NOTIFY_FD", index),
               *release = setting("VIGIL_TEST_RELEASE_FD", index);
    if (notify == NULL || release == NULL) abort();
    char token[80] = {0};
    (void)snprintf(token, sizeof(token), "%s:%u", name, hits[index]);
    if (write(atoi(notify), token, sizeof(token)) != (ssize_t)sizeof(token)) abort();
    char byte;
    ssize_t n;
    do {
        n = read(atoi(release), &byte, 1);
    } while (n < 0 && errno == EINTR);
    if (n != 1) abort();
    injected = byte;
    /* Release the selected worker and disable further pauses for this selection.
     * Test fixture only; production does not link this dispatcher. */
    disabled[index] = byte == 'D';
}
void vigil_test_hook(const char *name)
{
    /* Independent pipes let the fixture hold both owners at different cuts.
     * No environment mutation occurs while workers are running. */
    for (unsigned i = 0; i < 9U; ++i) hook(name, i);
}
int vigil_test_fault(const char *name)
{
    bool once = (injected == 'N' && strcmp(name, "directory_sync") == 0) ||
                (injected == 'P' && strcmp(name, "usage_prepare_nomem") == 0) ||
                (injected == 'T' && strcmp(name, "usage_step_nomem") == 0) ||
                (injected == 'U' && strcmp(name, "usage_done_nomem") == 0);
    if (once) {
        injected = '\0';
        return 1;
    }
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
