/* Same probe compiled against both pinned trees. No CAN, queue or database. */
#include "vigil/monitor.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define SAMPLES 100000U
#define WARMUP 10000U
static uint64_t ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0 || t.tv_sec < 0) abort();
    return (uint64_t)t.tv_sec * 1000000000U + (uint64_t)t.tv_nsec;
}
static int compare(const void *a, const void *b)
{
    uint64_t x=*(const uint64_t *)a, y=*(const uint64_t *)b;
    return (x>y)-(x<y);
}
static void distribution(const char *name, uint64_t *values)
{
    qsort(values,SAMPLES,sizeof(*values),compare);
    printf("\"%s\":{\"n\":%u,\"p50_ns\":%" PRIu64 ",\"p95_ns\":%" PRIu64
           ",\"p99_ns\":%" PRIu64 ",\"max_ns\":%" PRIu64 "}", name,SAMPLES,
           values[SAMPLES/2U-1U],values[SAMPLES*95U/100U-1U],values[SAMPLES*99U/100U-1U],values[SAMPLES-1U]);
}
int main(void)
{
    static uint64_t values[SAMPLES], overhead[SAMPLES];
    uint8_t frames[256][6];
    for (unsigned i=0;i<256U;++i) {
        frames[i][0]=0; frames[i][1]=0x98; frames[i][2]=8;
        frames[i][3]=0; frames[i][4]=(uint8_t)i;
        if (!vigil_crc8(frames[i],5,&frames[i][5])) abort();
    }
    struct vigil_config config;
    vigil_config_defaults(&config);
    config.pods[0].source[0].rate_per_s=0;
    struct vigil_monitor m;
    if (!vigil_monitor_init(&m,&config,0,NULL,NULL)) abort();
    for (unsigned i=0;i<WARMUP;++i)
        if (!vigil_monitor_frame(&m,0x101,false,frames[i%256U],6,i)) abort();
    for (unsigned i=0;i<SAMPLES;++i) {
        uint64_t start=ns();
        if (!vigil_monitor_frame(&m,0x101,false,frames[(i+WARMUP)%256U],6,i+WARMUP)) abort();
        values[i]=ns()-start;
        start=ns(); overhead[i]=ns()-start;
    }
    const unsigned count=1000000U;
    uint64_t start=ns();
    for (unsigned i=0;i<count;++i)
        if (!vigil_monitor_frame(&m,0x101,false,frames[(i+WARMUP+SAMPLES)%256U],6,i+WARMUP+SAMPLES)) abort();
    uint64_t duration=ns()-start;
    if (m.counters.valid_readings!=WARMUP+SAMPLES+count || m.counters.frames_rejected!=0) abort();
    printf("{"); distribution("frame",values); printf(","); distribution("clock_pair",overhead);
    printf(",\"warmup\":%u,\"bulk_count\":%u,\"bulk_ns\":%" PRIu64 ",\"bulk_frames_s\":%.3f}\n",
           WARMUP,count,duration,(double)count*1e9/(double)duration);
    return 0;
}
