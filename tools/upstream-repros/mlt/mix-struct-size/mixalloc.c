// Every "mix" transition embeds two 192000-sample x 6-channel float buffers in its struct
// (transition_mix.c: src_buffer, dest_buffer), a 9.2 MB calloc() per instance. glibc
// serves the first ones from fresh mmap pages, but once one is freed its dynamic mmap
// threshold rises above 9.2 MB and later ones come from the heap and are zeroed in full.
// An app that rebuilds a graph with many mixes (one per track and per audio crossfade)
// then pays for every byte. Round 3 below: 500 mixes, 2.3 s and 4.4 GB resident.
//
// Build:  cc mixalloc.c -o mixalloc $(pkg-config --cflags --libs mlt-framework-7)
// Run:    ./mixalloc
#include <framework/mlt.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static long rss_kb(void)
{
    long kb = -1;
    char line[256];
    FILE *f = fopen("/proc/self/status", "r");
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, "VmRSS:", 6))
            sscanf(line + 6, "%ld", &kb);
    if (f)
        fclose(f);
    return kb;
}

int main(void)
{
    enum { N = 500 };
    static mlt_transition mixes[N];
    mlt_properties keep[5] = {0};
    mlt_factory_init(NULL);
    mlt_profile profile = mlt_profile_init(NULL);
    for (int round = 1; round <= 4; round++) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < N; i++)
            mixes[i] = mlt_factory_transition(profile, "mix", NULL);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long rss = rss_kb();
        // Something small that outlives this round (as the next graph's objects do in an
        // editor that rebuilds), so the heap can't be trimmed below the freed mixes.
        keep[round] = mlt_properties_new();
        for (int i = 0; i < N; i++)
            mlt_transition_close(mixes[i]);
        printf("round %d: %d mix transitions created in %.0f ms, RSS while alive %ld MB, after close %ld MB\n",
               round, N, (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6, rss / 1024,
               rss_kb() / 1024);
    }
    for (int i = 1; i <= 4; i++)
        mlt_properties_close(keep[i]);
    mlt_profile_close(profile);
    mlt_factory_close();
    return 0;
}
