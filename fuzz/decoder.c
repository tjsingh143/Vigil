/* Input: 4 little-endian raw CAN ID bytes, 1 FD flag byte, then payload.
 * No sockets, state, clocks, allocation, or monitoring policy in this target. */
#include "vigil/protocol.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 5U) return 0;
    uint32_t id = (uint32_t)data[0] | ((uint32_t)data[1] << 8U) |
                  ((uint32_t)data[2] << 16U) | ((uint32_t)data[3] << 24U);
    struct vigil_message message;
    unsigned char before[sizeof(message)];
    memset(&message, 0xa5, sizeof(message));
    memcpy(before, &message, sizeof(message));
    enum vigil_result result = vigil_decode(id, (data[4] & 1U) != 0U,
                                            data + 5, size - 5U, &message);
    assert(result >= VIGIL_OK && result < VIGIL_RESULT_COUNT);
    if (result != VIGIL_OK) {
        /* Compare object bytes only on failure: padding is never a success oracle. */
        assert(memcmp(before, &message, sizeof(message)) == 0);
    } else {
        assert(data[4] % 2U == 0U);
        assert(message.pod_id >= 1U && message.pod_id <= 127U);
        if (message.kind == VIGIL_READING) {
            assert(size == 11U && id == 0x100U + message.pod_id);
            assert(message.channel <= VIGIL_DOOR && message.status <= VIGIL_STATUS_FAULT);
            assert(message.channel != VIGIL_DOOR || message.value == 0 || message.value == 1);
            assert(message.pod_state == 0U && message.uptime_s == 0U);
        } else {
            assert(message.kind == VIGIL_HEARTBEAT);
            assert(size == 12U && id == 0x700U + message.pod_id);
            assert(message.pod_state <= VIGIL_POD_FAULT);
            assert(message.channel == 0U && message.value == 0 && message.status == 0U);
        }
    }
    memcpy(&message, before, sizeof(message));
    assert(vigil_decode(id, false, NULL, size - 5U, &message) == VIGIL_ERR_ARGUMENT);
    assert(memcmp(before, &message, sizeof(message)) == 0);
    assert(vigil_decode(id, false, data + 5, size - 5U, NULL) == VIGIL_ERR_ARGUMENT);
    return 0;
}
