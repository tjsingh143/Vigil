#include "vigil/protocol.h"

bool vigil_crc8(const uint8_t *data, size_t length, uint8_t *out)
{
    uint8_t crc = 0;
    if (out == NULL || (data == NULL && length != 0U)) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8U; ++bit) {
            crc = (uint8_t)(((unsigned)crc << 1U) ^
                            ((crc & 0x80U) != 0U ? 0x07U : 0U));
        }
    }
    *out = crc;
    return true;
}

enum vigil_result vigil_decode(uint32_t raw_id, bool is_fd,
                               const uint8_t *data, size_t length,
                               struct vigil_message *out)
{
    struct vigil_message msg = {0};
    size_t expected;
    uint8_t crc;
    if (data == NULL || out == NULL) {
        return VIGIL_ERR_ARGUMENT;
    }
    if (is_fd || (raw_id & UINT32_C(0xe0000000)) != 0U) {
        return VIGIL_ERR_FORMAT;
    }
    if (raw_id >= 0x101U && raw_id <= 0x17fU) {
        msg.kind = VIGIL_READING;
        msg.pod_id = (uint8_t)(raw_id - 0x100U);
        expected = 6;
    } else if (raw_id >= 0x701U && raw_id <= 0x77fU) {
        msg.kind = VIGIL_HEARTBEAT;
        msg.pod_id = (uint8_t)(raw_id - 0x700U);
        expected = 7;
    } else {
        return VIGIL_ERR_ID;
    }
    if (length != expected) {
        return VIGIL_ERR_LENGTH;
    }
    (void)vigil_crc8(data, length - 1U, &crc);
    if (crc != data[length - 1U]) {
        return VIGIL_ERR_CRC;
    }
    if (msg.kind == VIGIL_READING) {
        uint32_t raw_value = (uint32_t)data[1] | ((uint32_t)data[2] << 8U);
        /* Explicit subtraction avoids implementation-defined unsigned->signed
         * conversion for values outside int16_t's positive range. */
        int32_t signed_value = raw_value <= INT16_MAX ? (int32_t)raw_value :
                               (int32_t)raw_value - INT32_C(65536);
        if (data[0] > VIGIL_DOOR) {
            return VIGIL_ERR_CHANNEL;
        }
        if (data[3] > VIGIL_STATUS_FAULT) {
            return VIGIL_ERR_STATUS;
        }
        if (data[0] == VIGIL_DOOR && signed_value != 0 && signed_value != 1) {
            return VIGIL_ERR_VALUE;
        }
        msg.channel = data[0];
        msg.value = (int16_t)signed_value;
        msg.status = data[3];
        msg.counter = data[4];
    } else {
        if (data[0] > VIGIL_POD_FAULT) {
            return VIGIL_ERR_STATE;
        }
        msg.pod_state = data[0];
        msg.uptime_s = (uint32_t)data[1] | ((uint32_t)data[2] << 8U) |
                       ((uint32_t)data[3] << 16U) | ((uint32_t)data[4] << 24U);
        msg.counter = data[5];
    }
    *out = msg;
    return VIGIL_OK;
}

const char *vigil_result_name(enum vigil_result result)
{
    static const char *const names[] = {
        "ok", "argument", "format", "id", "length", "crc", "channel",
        "status", "value", "state"
    };
    if ((unsigned)result >= (unsigned)VIGIL_RESULT_COUNT) {
        return "unknown";
    }
    return names[result];
}

struct vigil_counter_result vigil_counter_check(bool initialized,
                                               uint8_t previous,
                                               uint8_t current)
{
    const uint8_t delta = (uint8_t)((unsigned)current + 256U - previous);
    if (!initialized) {
        return (struct vigil_counter_result){VIGIL_SEQ_FIRST, 0, true};
    }
    if (delta == 0U) {
        return (struct vigil_counter_result){VIGIL_SEQ_REPEAT, 0, false};
    }
    if (delta >= 128U) {
        return (struct vigil_counter_result){VIGIL_SEQ_OLD, 0, false};
    }
    return (struct vigil_counter_result){delta == 1U ? VIGIL_SEQ_NEXT :
                                         VIGIL_SEQ_GAP,
                                         (uint8_t)(delta - 1U), true};
}
