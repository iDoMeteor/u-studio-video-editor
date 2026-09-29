// mlt_factory.c sets every service's "_unique_id" to ++unique_id on a plain static int
// (set_common_properties()), so services created on two threads at once can get the
// same id. The id keys per-frame data (mlt_frame_unique_properties(), mlt_filter.c,
// filter_panner.c, filter_shape.c) and movit's chain fingerprints.
// Expected: 0 duplicates. Actual (MLT 7.40.0, 16 cores): 54 to 749 per run.
//
// Build:  cc uniqueid.c -o uniqueid -pthread $(pkg-config --cflags --libs mlt-framework-7)
// Run:    ./uniqueid
#include <framework/mlt.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { THREADS = 8, PER_THREAD = 20000 };
static mlt_profile profile;
static int ids[THREADS][PER_THREAD];
static pthread_barrier_t barrier;

static void *create(void *arg)
{
    int *out = arg;
    pthread_barrier_wait(&barrier);
    for (int i = 0; i < PER_THREAD; i++) {
        mlt_filter f = mlt_factory_filter(profile, "brightness", NULL);
        out[i] = mlt_properties_get_int(MLT_FILTER_PROPERTIES(f), "_unique_id");
        mlt_filter_close(f);
    }
    return NULL;
}

static int cmp(const void *a, const void *b)
{
    return *(const int *) a - *(const int *) b;
}

int main(void)
{
    mlt_factory_init(NULL);
    profile = mlt_profile_init(NULL);
    mlt_filter_close(mlt_factory_filter(profile, "brightness", NULL)); // load the module first
    pthread_barrier_init(&barrier, NULL, THREADS);
    pthread_t threads[THREADS];
    for (int i = 0; i < THREADS; i++)
        pthread_create(&threads[i], NULL, create, ids[i]);
    for (int i = 0; i < THREADS; i++)
        pthread_join(threads[i], NULL);
    int *all = &ids[0][0], n = THREADS * PER_THREAD, dup = 0;
    qsort(all, n, sizeof *all, cmp);
    for (int i = 1; i < n; i++)
        dup += all[i] == all[i - 1];
    printf("%d services created, %d duplicate _unique_id values\n", n, dup);
    mlt_profile_close(profile);
    mlt_factory_close();
    return 0;
}
