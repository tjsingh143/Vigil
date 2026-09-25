#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "vigil/protocol.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static const uint8_t temperature[] = {0, 0x98, 8, 0, 0, 0xb7};
static const uint8_t heartbeat[] = {1, 0, 0, 0, 0, 0, 0x29};

static void reject(uint32_t id, bool fd, const uint8_t *data, size_t length,
                   enum vigil_result expected)
{
    struct vigil_message out;
    unsigned char saved[sizeof(out)];
    memset(&out, 0xa5, sizeof(out));
    memcpy(saved, &out, sizeof(out));
    assert_int_equal(vigil_decode(id, fd, data, length, &out), expected);
    assert_memory_equal(saved, &out, sizeof(out));
}

static void repair_crc(uint8_t *data, size_t length)
{
    assert_true(vigil_crc8(data, length - 1U, &data[length - 1U]));
}

static void test_crc_known(void **state)
{
    (void)state;
    uint8_t crc = 0xff;
    assert_true(vigil_crc8((const uint8_t *)"123456789", 9, &crc));
    assert_int_equal(crc, 0xf4);
    assert_true(vigil_crc8(NULL, 0, &crc));
    assert_int_equal(crc, 0);
    assert_true(vigil_crc8(temperature, 5, &crc));
    assert_int_equal(crc, 0xb7);
    assert_true(vigil_crc8(temperature, 6, &crc));
    assert_int_equal(crc, 0); /* zero residue includes the appended CRC */
}

static void test_crc_arguments(void **state)
{
    (void)state;
    uint8_t crc = 0xab;
    assert_false(vigil_crc8(NULL, 1, &crc));
    assert_int_equal(crc, 0xab);
    assert_false(vigil_crc8(temperature, 6, NULL));
}

static void test_shared_vectors(void **state)
{
    (void)state;
    FILE *file = fopen(VIGIL_VECTORS, "r");
    assert_non_null(file);
    char line[256];
    unsigned rows = 0;
    while (fgets(line, sizeof(line), file) != NULL) {
        if (line[0] == '#') continue;
        unsigned id, kind, pod, channel, status, counter, pod_state;
        int value;
        uint32_t uptime;
        char hex[17];
        assert_int_equal(sscanf(line, "%x %16s %u %u %u %d %u %u %u %" SCNu32,
                               &id, hex, &kind, &pod, &channel, &value,
                               &status, &counter, &pod_state, &uptime), 10);
        size_t length = strlen(hex) / 2U;
        uint8_t data[8];
        for (size_t i = 0; i < length; ++i) {
            unsigned byte;
            assert_int_equal(sscanf(&hex[i * 2U], "%2x", &byte), 1);
            data[i] = (uint8_t)byte;
        }
        struct vigil_message msg;
        assert_int_equal(vigil_decode(id, false, data, length, &msg), VIGIL_OK);
        assert_int_equal(msg.kind, kind);
        assert_int_equal(msg.pod_id, pod);
        assert_int_equal(msg.channel, channel);
        assert_int_equal(msg.value, value);
        assert_int_equal(msg.status, status);
        assert_int_equal(msg.counter, counter);
        assert_int_equal(msg.pod_state, pod_state);
        assert_int_equal(msg.uptime_s, uptime);
        ++rows;
    }
    assert_false(ferror(file));
    assert_int_equal(fclose(file), 0);
    assert_int_equal(rows, 10);
}

static void test_lengths(void **state)
{
    (void)state;
    /* A one-byte allocation proves wrong lengths are rejected before access. */
    const uint8_t byte = 0;
    for (size_t length = 0; length <= 64; ++length) {
        if (length != 6) reject(0x101, false, &byte, length, VIGIL_ERR_LENGTH);
        if (length != 7) reject(0x701, false, &byte, length, VIGIL_ERR_LENGTH);
    }
    reject(0x101, false, &byte, SIZE_MAX, VIGIL_ERR_LENGTH);
}

static void test_all_standard_ids(void **state)
{
    (void)state;
    for (uint32_t id = 0; id <= 0x7ff; ++id) {
        struct vigil_message msg;
        if (id >= 0x101 && id <= 0x17f) {
            assert_int_equal(vigil_decode(id, false, temperature, 6, &msg), VIGIL_OK);
            assert_int_equal(msg.pod_id, id - 0x100);
        } else if (id >= 0x701 && id <= 0x77f) {
            assert_int_equal(vigil_decode(id, false, heartbeat, 7, &msg), VIGIL_OK);
            assert_int_equal(msg.pod_id, id - 0x700);
        } else {
            reject(id, false, temperature, 6, VIGIL_ERR_ID);
        }
    }
    reject(0x800, false, temperature, 6, VIGIL_ERR_ID);
    reject(0x1fffffff, false, temperature, 6, VIGIL_ERR_ID);
}

static void test_frame_formats(void **state)
{
    (void)state;
    reject(0x80000101, false, temperature, 6, VIGIL_ERR_FORMAT);
    reject(0x40000101, false, temperature, 6, VIGIL_ERR_FORMAT);
    reject(0x20000101, false, temperature, 6, VIGIL_ERR_FORMAT);
    reject(UINT32_MAX, false, temperature, 6, VIGIL_ERR_FORMAT);
    reject(0x101, true, temperature, 6, VIGIL_ERR_FORMAT);
}

static void test_single_bit_corruption(void **state)
{
    (void)state;
    /* Every one-bit mutation, including CRC itself, must fail CRC. */
    for (unsigned kind = 0; kind < 2; ++kind) {
        size_t length = kind == 0 ? 6U : 7U;
        for (size_t bit = 0; bit < length * 8U; ++bit) {
            uint8_t bytes[7];
            memcpy(bytes, kind == 0 ? temperature : heartbeat, length);
            bytes[bit / 8U] ^= (uint8_t)(1U << (bit % 8U));
            reject(kind == 0 ? 0x101U : 0x701U, false, bytes, length, VIGIL_ERR_CRC);
        }
    }
}

static void test_channel_status_state(void **state)
{
    (void)state;
    for (unsigned value = 2; value <= 255; ++value) {
        uint8_t bytes[7];
        memcpy(bytes, temperature, 6);
        bytes[0] = (uint8_t)value;
        repair_crc(bytes, 6);
        reject(0x101, false, bytes, 6, VIGIL_ERR_CHANNEL);
        memcpy(bytes, temperature, 6);
        bytes[3] = (uint8_t)value;
        repair_crc(bytes, 6);
        reject(0x101, false, bytes, 6, VIGIL_ERR_STATUS);
        if (value >= 3) {
            memcpy(bytes, heartbeat, 7);
            bytes[0] = (uint8_t)value;
            repair_crc(bytes, 7);
            reject(0x701, false, bytes, 7, VIGIL_ERR_STATE);
        }
    }
}

static void test_int16_domain_and_door(void **state)
{
    (void)state;
    /* Exhaust the wire domain; includes -32768, -1, 0, 1, 32767. */
    for (int32_t value = INT16_MIN; value <= INT16_MAX; ++value) {
        uint16_t wire = (uint16_t)value;
        uint8_t bytes[6] = {0, (uint8_t)wire, (uint8_t)(wire >> 8U), 0, 0, 0};
        struct vigil_message msg;
        repair_crc(bytes, 6);
        assert_int_equal(vigil_decode(0x101, false, bytes, 6, &msg), VIGIL_OK);
        assert_int_equal(msg.value, value);
        bytes[0] = 1;
        repair_crc(bytes, 6);
        if (value == 0 || value == 1) {
            assert_int_equal(vigil_decode(0x101, false, bytes, 6, &msg), VIGIL_OK);
            assert_int_equal(msg.value, value);
        } else reject(0x101, false, bytes, 6, VIGIL_ERR_VALUE);
    }
}

static void test_null_arguments(void **state)
{
    (void)state;
    reject(0x101, false, NULL, 6, VIGIL_ERR_ARGUMENT);
    reject(0x101, false, NULL, 0, VIGIL_ERR_ARGUMENT);
    assert_int_equal(vigil_decode(0x101, false, temperature, 6, NULL), VIGIL_ERR_ARGUMENT);
}

static void test_result_names(void **state)
{
    (void)state;
    assert_string_equal(vigil_result_name(VIGIL_ERR_CRC), "crc");
    assert_string_equal(vigil_result_name((enum vigil_result)-1), "unknown");
    assert_string_equal(vigil_result_name(VIGIL_RESULT_COUNT), "unknown");
    for (int i = 0; i < VIGIL_RESULT_COUNT; ++i)
        assert_string_not_equal(vigil_result_name((enum vigil_result)i), "unknown");
}

static void sequence(bool initialized, uint8_t previous, uint8_t current,
                     enum vigil_sequence expected, bool accepted, uint8_t lost)
{
    struct vigil_counter_result result = vigil_counter_check(initialized, previous, current);
    assert_int_equal(result.sequence, expected);
    assert_int_equal(result.accept, accepted);
    assert_int_equal(result.lost, lost);
}

static void test_counter_examples(void **state)
{
    (void)state;
    sequence(false, 99, 99, VIGIL_SEQ_FIRST, true, 0);
    sequence(true, 0, 1, VIGIL_SEQ_NEXT, true, 0);
    sequence(true, 99, 99, VIGIL_SEQ_REPEAT, false, 0);
    sequence(true, 10, 13, VIGIL_SEQ_GAP, true, 2);
    sequence(true, 255, 0, VIGIL_SEQ_NEXT, true, 0);
    sequence(true, 254, 1, VIGIL_SEQ_GAP, true, 2);
    sequence(true, 100, 99, VIGIL_SEQ_OLD, false, 0);
    sequence(true, 0, 127, VIGIL_SEQ_GAP, true, 126);
    sequence(true, 0, 128, VIGIL_SEQ_OLD, false, 0);
    sequence(true, 0, 255, VIGIL_SEQ_OLD, false, 0);
}

static void test_counter_all_baselines(void **state)
{
    (void)state;
    for (unsigned base = 0; base <= 255; ++base) {
        unsigned accepted = 0, repeated = 0, old = 0;
        for (unsigned current = 0; current <= 255; ++current) {
            struct vigil_counter_result result = vigil_counter_check(true,
                                             (uint8_t)base, (uint8_t)current);
            accepted += result.accept ? 1U : 0U;
            repeated += result.sequence == VIGIL_SEQ_REPEAT ? 1U : 0U;
            old += result.sequence == VIGIL_SEQ_OLD ? 1U : 0U;
            sequence(false, (uint8_t)base, (uint8_t)current, VIGIL_SEQ_FIRST, true, 0);
        }
        assert_int_equal(accepted, 127);
        assert_int_equal(repeated, 1);
        assert_int_equal(old, 128);
    }
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_crc_known), cmocka_unit_test(test_crc_arguments),
        cmocka_unit_test(test_shared_vectors), cmocka_unit_test(test_lengths),
        cmocka_unit_test(test_all_standard_ids), cmocka_unit_test(test_frame_formats),
        cmocka_unit_test(test_single_bit_corruption),
        cmocka_unit_test(test_channel_status_state),
        cmocka_unit_test(test_int16_domain_and_door),
        cmocka_unit_test(test_null_arguments), cmocka_unit_test(test_result_names),
        cmocka_unit_test(test_counter_examples),
        cmocka_unit_test(test_counter_all_baselines)
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
