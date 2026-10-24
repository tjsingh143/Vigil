#ifndef VIGIL_OUTBOX_H
#define VIGIL_OUTBOX_H
#include "vigil/queue.h"
#define VIGIL_OUTBOX_RECORDS 65536U
#define VIGIL_OUTBOX_BATCHES 4096U
#define VIGIL_OUTBOX_BYTES (64U * 1024U * 1024U)
#define VIGIL_PAYLOAD_MAX 160000U
#define VIGIL_OUTBOX_DEFAULT "/var/lib/vigil/outbox.sqlite3"
enum vigil_store_result {
    VIGIL_STORE_OK,
    VIGIL_STORE_FULL,
    VIGIL_STORE_BUSY,
    VIGIL_STORE_IO_UNKNOWN,
    VIGIL_STORE_CORRUPT,
    VIGIL_STORE_INVALID,
    VIGIL_STORE_NOMEM,
    VIGIL_STORE_EXHAUSTED,
    VIGIL_STORE_READONLY
};
struct vigil_capture {
    struct vigil_record records[VIGIL_BATCH_SIZE], alarms[VIGIL_ALARM_CAPACITY];
    size_t count, alarm_count;
    struct vigil_queue_stats stats;
    uint64_t revision, ticket;
    bool recovery;
};
struct vigil_outbox_usage {
    uint64_t records, batches, bytes, admitted, readings, retired;
};
struct vigil_local_commit {
    uint64_t order_id, batch_id, first_seq, records, readings;
};
struct vigil_replay_batch {
    struct vigil_local_commit id;
    char run_id[37];
    size_t bytes;
    uint64_t revision, reading_drops, event_drops;
    bool gap, snapshot;
    char payload[VIGIL_PAYLOAD_MAX + 1U];
};
struct vigil_outbox;
/* Single-thread owner API. Paths must be explicitly provisioned on ext4/XFS.
 * Bootstrap only creates a new database in an otherwise empty private directory.
 * Normal open never creates or silently resets history. */
enum vigil_store_result vigil_outbox_open(struct vigil_outbox **out, const char *path,
                                          bool bootstrap);
enum vigil_store_result vigil_outbox_begin_run(struct vigil_outbox *o);
enum vigil_store_result vigil_outbox_append(struct vigil_outbox *o, const struct vigil_capture *c,
                                            struct vigil_local_commit *committed);
/* Empty head is OK with bytes=0; statements end before returning. */
enum vigil_store_result vigil_outbox_peek(struct vigil_outbox *o, struct vigil_replay_batch *b);
enum vigil_store_result vigil_outbox_retire(struct vigil_outbox *o, const struct vigil_replay_batch *b);
enum vigil_store_result vigil_outbox_checkpoint(struct vigil_outbox *o, bool truncate);
enum vigil_store_result vigil_outbox_close(struct vigil_outbox *o);
void vigil_outbox_usage(const struct vigil_outbox *o, struct vigil_outbox_usage *usage);
const char *vigil_outbox_run_id(const struct vigil_outbox *o);
int vigil_outbox_error(const struct vigil_outbox *o);
const char *vigil_store_name(enum vigil_store_result result);
#endif
