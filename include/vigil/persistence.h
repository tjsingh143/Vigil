#ifndef VIGIL_PERSISTENCE_H
#define VIGIL_PERSISTENCE_H
#include "vigil/queue.h"
#include <limits.h>
struct vigil_persistence {
    struct vigil_queue queue;
    pthread_t thread, replay_thread;
    struct vigil_replay *replay;
    char run_id[37], outbox_path[PATH_MAX];
    char host[108], port[6], database[64], user[64];
    bool network, startup_done, startup_ok;
};
/* The worker alone owns SQLite and directory synchronization. Start waits only
 * during setup; ingestion never waits for a commit. The separate replay worker alone owns libpq. */
bool vigil_persistence_start(struct vigil_persistence *p, size_t capacity, const char *outbox_path);
void vigil_persistence_stop(struct vigil_persistence *p);
void vigil_persistence_destroy(struct vigil_persistence *p);
#endif
