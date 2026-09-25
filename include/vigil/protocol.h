#ifndef VIGIL_PROTOCOL_H
#define VIGIL_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum vigil_kind { VIGIL_READING = 1, VIGIL_HEARTBEAT = 2 };
enum vigil_channel { VIGIL_TEMPERATURE = 0, VIGIL_DOOR = 1 };
enum vigil_status { VIGIL_STATUS_OK = 0, VIGIL_STATUS_FAULT = 1 };
enum vigil_pod_state { VIGIL_POD_BOOTING = 0, VIGIL_POD_OPERATIONAL = 1,
                       VIGIL_POD_FAULT = 2 };
enum vigil_result {
    VIGIL_OK = 0,
    VIGIL_ERR_ARGUMENT,
    VIGIL_ERR_FORMAT,
    VIGIL_ERR_ID,
    VIGIL_ERR_LENGTH,
    VIGIL_ERR_CRC,
    VIGIL_ERR_CHANNEL,
    VIGIL_ERR_STATUS,
    VIGIL_ERR_VALUE,
    VIGIL_ERR_STATE,
    VIGIL_RESULT_COUNT
};

struct vigil_message {
    enum vigil_kind kind;
    uint8_t pod_id;
    uint8_t counter;
    uint8_t channel;
    int16_t value;
    uint8_t status;
    uint8_t pod_state;
    uint32_t uptime_s;
};

/* CRC-8/SMBUS. NULL data is permitted only for zero length. */
bool vigil_crc8(const uint8_t *data, size_t length, uint8_t *out);
/* raw_id includes SocketCAN flags. Output is unchanged on any error.
 * data must reference at least length readable bytes. FD is never accepted. */
enum vigil_result vigil_decode(uint32_t raw_id, bool is_fd,
                               const uint8_t *data, size_t length,
                               struct vigil_message *out);
const char *vigil_result_name(enum vigil_result result);

enum vigil_sequence {
    VIGIL_SEQ_FIRST, VIGIL_SEQ_NEXT, VIGIL_SEQ_GAP,
    VIGIL_SEQ_REPEAT, VIGIL_SEQ_OLD
};
struct vigil_counter_result {
    enum vigil_sequence sequence;
    uint8_t lost;
    bool accept;
};
/* Pure classification, no mutation. Caller updates its baseline only on accept.
 * delta 128..255 is old/ambiguous. See docs/PROTOCOL.md for limitations. */
struct vigil_counter_result vigil_counter_check(bool initialized,
                                               uint8_t previous,
                                               uint8_t current);
#endif
