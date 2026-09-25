#include "vigil/protocol.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int decode_print(uint32_t id, const uint8_t *data, size_t length)
{
    struct vigil_message msg;
    enum vigil_result result = vigil_decode(id, false, data, length, &msg);
    if (result != VIGIL_OK) {
        printf("{\"result\":\"%s\"}\n", vigil_result_name(result));
        return 1;
    }
    printf("{\"result\":\"ok\",\"kind\":%u,\"pod\":%u,\"channel\":%u,"
           "\"value\":%d,\"status\":%u,\"counter\":%u,\"state\":%u,"
           "\"uptime_s\":%" PRIu32 "}\n",
           (unsigned)msg.kind, (unsigned)msg.pod_id, (unsigned)msg.channel,
           (int)msg.value, (unsigned)msg.status, (unsigned)msg.counter,
           (unsigned)msg.pod_state, msg.uptime_s);
    return 0;
}

static bool parse_number(const char *s, int base, unsigned long maximum,
                         unsigned long *out)
{
    char *end;
    if (s[0] == '\0' || s[0] == '-' || s[0] == '+' ||
        isspace((unsigned char)s[0]) != 0) {
        return false;
    }
    errno = 0;
    unsigned long value = strtoul(s, &end, base);
    if (errno != 0 || *end != '\0' || value > maximum) {
        return false;
    }
    *out = value;
    return true;
}

static int hex_digit(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static int64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int listen_frames(const char *interface, unsigned long count)
{
    unsigned index = if_nametoindex(interface);
    if (index == 0U || index > INT_MAX) {
        fprintf(stderr, "invalid CAN interface: %s\n", interface);
        return 2;
    }
    int fd = socket(PF_CAN, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, CAN_RAW);
    if (fd < 0) { perror("socket"); return 2; }
    struct sockaddr_can address = {0};
    address.can_family = AF_CAN;
    address.can_ifindex = (int)index;
    if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        perror("bind");
        (void)close(fd);
        return 2;
    }
    /* Diagnostic probe only: bounded overall timeout, no daemon state. */
    const int64_t start = monotonic_ms();
    if (start < 0) { (void)close(fd); return 2; }
    fprintf(stderr, "ready interface=%s\n", interface);
    fflush(stderr);
    int status = 0;
    for (unsigned long received = 0; received < count;) {
        int64_t now = monotonic_ms();
        if (now < 0 || now - start >= 10000) {
            fprintf(stderr, "receive timeout\n");
            status = 2;
            break;
        }
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int ready = poll(&pfd, 1, (int)(10000 - (now - start)));
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            fprintf(stderr, "CAN poll failed\n"); status = 2; break;
        }
        if (ready == 0) continue;
        struct can_frame frame;
        ssize_t size = read(fd, &frame, sizeof(frame));
        if (size < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (size != (ssize_t)sizeof(frame)) {
            fprintf(stderr, "invalid SocketCAN read\n"); status = 2; break;
        }
        if (decode_print(frame.can_id, frame.data, frame.len) != 0) status = 1;
        fflush(stdout);
        ++received;
    }
    if (close(fd) != 0) { perror("close"); return 2; }
    return status;
}

int main(int argc, char **argv)
{
    unsigned long number;
    if (argc == 4 && strcmp(argv[1], "--listen") == 0) {
        if (!parse_number(argv[3], 10, 1000000, &number) || number == 0U) {
            fprintf(stderr, "count must be 1..1000000\n"); return 2;
        }
        return listen_frames(argv[2], number);
    }
    if (argc != 3 || !parse_number(argv[1], 16, UINT32_MAX, &number)) {
        fprintf(stderr, "usage: vigil-decode HEX_ID HEX_DATA\n"
                        "       vigil-decode --listen INTERFACE COUNT\n");
        return 2;
    }
    size_t digits = strlen(argv[2]);
    uint8_t data[8] = {0};
    if (digits > 16U || digits % 2U != 0U) {
        fprintf(stderr, "data must contain 0..8 complete hex bytes\n"); return 2;
    }
    for (size_t i = 0; i < digits / 2U; ++i) {
        int high = hex_digit(argv[2][i * 2U]);
        int low = hex_digit(argv[2][i * 2U + 1U]);
        if (high < 0 || low < 0) { fprintf(stderr, "invalid hex\n"); return 2; }
        data[i] = (uint8_t)(high * 16 + low);
    }
    return decode_print((uint32_t)number, data, digits / 2U);
}
