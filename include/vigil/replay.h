#ifndef VIGIL_REPLAY_H
#define VIGIL_REPLAY_H
#include "vigil/outbox.h"
enum vigil_remote_state { VIGIL_REMOTE_DISCONNECTED, VIGIL_REMOTE_PENDING,
    VIGIL_REMOTE_UNKNOWN, VIGIL_REMOTE_CONFIRMED, VIGIL_REMOTE_POISON };
enum vigil_slot_state { VIGIL_SLOT_EMPTY, VIGIL_SLOT_READY, VIGIL_SLOT_SENDING, VIGIL_SLOT_RESULT };
struct vigil_replay {
    pthread_mutex_t mutex;
    enum vigil_slot_state state;
    struct vigil_replay_batch batch;
    bool success, poison, stop;
};
struct vigil_persistence;
void *vigil_replay_worker(void *context);
const char *vigil_remote_name(unsigned state);
#endif
