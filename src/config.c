#include "vigil/monitor.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MS UINT64_C(86400000)

void vigil_config_defaults(struct vigil_config *config)
{
    *config = (struct vigil_config){.tick_ms = 100, .status_ms = 1000,
        .pod_count = 1, .pods = {{.id = 1, .heartbeat_ms = 3000,
        .source = {
            {.enabled = true, .timeout_ms = 3000, .minimum = -4000,
             .maximum = 8500, .rate_per_s = 500, .flat_ms = 60000,
             .high = 2700, .clear = 2600},
            {.enabled = true, .timeout_ms = 3000, .minimum = 0,
             .maximum = 1, .rate_per_s = 0, .flat_ms = 0,
             .high = 1, .clear = 0}}}}};
}

bool vigil_config_valid(const struct vigil_config *config)
{
    if (config == NULL || config->pod_count == 0 ||
        config->pod_count > VIGIL_MAX_PODS || config->tick_ms < 10 ||
        config->tick_ms > 1000 || config->status_ms > MAX_MS ||
        (config->status_ms != 0 && config->status_ms < config->tick_ms)) return false;
    for (size_t p = 0; p < config->pod_count; ++p) {
        const struct vigil_pod_config *pod = &config->pods[p];
        if (pod->id == 0 || pod->id > 127 || pod->heartbeat_ms < config->tick_ms ||
            pod->heartbeat_ms > MAX_MS) return false;
        for (size_t other = 0; other < p; ++other)
            if (config->pods[other].id == pod->id) return false;
        bool any = false;
        for (size_t c = 0; c < VIGIL_CHANNELS; ++c) {
            const struct vigil_source_config *s = &pod->source[c];
            if (!s->enabled) continue;
            any = true;
            if (s->timeout_ms < config->tick_ms || s->timeout_ms > MAX_MS ||
                s->minimum >= s->maximum || s->clear >= s->high ||
                s->clear < s->minimum || s->high > s->maximum ||
                s->rate_per_s > 65535U || s->flat_ms > MAX_MS ||
                (s->flat_ms != 0 && s->flat_ms < config->tick_ms) ||
                (c == VIGIL_DOOR && (s->minimum != 0 || s->maximum != 1)))
                return false;
        }
        if (!any) return false;
    }
    return true;
}

static bool number(const char *text, int64_t low, int64_t high, int64_t *out)
{
    if (*text == '\0' || *text == '+' ||
        (*text != '-' && (*text < '0' || *text > '9'))) return false;
    char *end;
    errno = 0;
    intmax_t value = strtoimax(text, &end, 10);
    if (errno != 0 || *end != '\0' || value < low || value > high) return false;
    *out = (int64_t)value;
    return true;
}

static bool parse_line(struct vigil_config *config, char **words, size_t count,
                        bool *tick_seen, bool *status_seen)
{
    int64_t n[10];
    if (count == 2 && (strcmp(words[0], "tick_ms") == 0 ||
                      strcmp(words[0], "status_ms") == 0)) {
        bool tick = strcmp(words[0], "tick_ms") == 0;
        bool *seen = tick ? tick_seen : status_seen;
        if (*seen || !number(words[1], 0, (int64_t)MAX_MS, &n[0])) return false;
        *seen = true;
        if (tick) config->tick_ms = (uint64_t)n[0];
        else config->status_ms = (uint64_t)n[0];
        return true;
    }
    if (count < 2 || !number(words[1], 1, 127, &n[0])) return false;
    size_t p = 0;
    while (p < config->pod_count && config->pods[p].id != (uint8_t)n[0]) ++p;
    if (strcmp(words[0], "pod") == 0 && count == 3) {
        if (p != config->pod_count || p == VIGIL_MAX_PODS ||
            !number(words[2], 1, (int64_t)MAX_MS, &n[1])) return false;
        config->pods[p].id = (uint8_t)n[0];
        config->pods[p].heartbeat_ms = (uint64_t)n[1];
        ++config->pod_count;
        return true;
    }
    if (strcmp(words[0], "source") != 0 || count != 10 ||
        p == config->pod_count) return false;
    size_t c;
    if (strcmp(words[2], "temperature") == 0) c = VIGIL_TEMPERATURE;
    else if (strcmp(words[2], "door") == 0) c = VIGIL_DOOR;
    else return false;
    if (config->pods[p].source[c].enabled) return false;
    const int64_t lows[] = {1, INT16_MIN, INT16_MIN, 0, 0, INT16_MIN, INT16_MIN};
    /* Seven numeric source fields: timeout min max rate flat high clear. */
    const int64_t highs[] = {(int64_t)MAX_MS, INT16_MAX, INT16_MAX, 65535,
                            (int64_t)MAX_MS, INT16_MAX, INT16_MAX};
    for (size_t i = 0; i < 7; ++i)
        if (!number(words[i + 3U], lows[i], highs[i], &n[i])) return false;
    config->pods[p].source[c] = (struct vigil_source_config){
        .enabled = true, .timeout_ms = (uint64_t)n[0],
        .minimum = (int16_t)n[1], .maximum = (int16_t)n[2],
        .rate_per_s = (uint32_t)n[3], .flat_ms = (uint64_t)n[4],
        .high = (int16_t)n[5], .clear = (int16_t)n[6]};
    return true;
}

bool vigil_config_load(const char *path, struct vigil_config *out,
                       char *error, size_t error_size)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        (void)snprintf(error, error_size, "cannot open config: %s", strerror(errno));
        return false;
    }
    struct vigil_config candidate = {.tick_ms = 100, .status_ms = 1000};
    bool tick_seen = false, status_seen = false, ok = true;
    size_t line_number = 0;
    char line[512];
    while (fgets(line, sizeof(line), file) != NULL) {
        ++line_number;
        if (strchr(line, '\n') == NULL && !feof(file)) { ok = false; break; }
        char *comment = strchr(line, '#');
        if (comment != NULL) *comment = '\0';
        char *words[12];
        size_t count = 0;
        char *save;
        char *word = strtok_r(line, " \t\r\n", &save);
        while (word != NULL && count < 12) {
            words[count++] = word;
            word = strtok_r(NULL, " \t\r\n", &save);
        }
        if (word != NULL || (count != 0 &&
            !parse_line(&candidate, words, count, &tick_seen, &status_seen))) {
            ok = false;
            break;
        }
    }
    if (ferror(file)) ok = false;
    if (fclose(file) != 0) ok = false;
    if (!ok) {
        (void)snprintf(error, error_size, "invalid config or read error at line %zu", line_number);
        return false;
    }
    if (!vigil_config_valid(&candidate)) {
        (void)snprintf(error, error_size, "invalid configuration limits or missing sources");
        return false;
    }
    *out = candidate;
    return true;
}
