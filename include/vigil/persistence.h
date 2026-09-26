#ifndef VIGIL_PERSISTENCE_H
#define VIGIL_PERSISTENCE_H
#include "vigil/queue.h"
struct vigil_persistence {
    struct vigil_queue queue;
    pthread_t thread;
    char run_id[37];
    char host[108], port[6], database[64], user[64];
    uint64_t started_ms, started_utc_ms;
    bool network;
};
/* Environment copied/validated before thread start. Libpq only in writer. */
bool vigil_persistence_start(struct vigil_persistence *p, size_t capacity);
void vigil_persistence_stop(struct vigil_persistence *p); /* bounded drain + join */
void vigil_persistence_destroy(struct vigil_persistence *p);
#endif
