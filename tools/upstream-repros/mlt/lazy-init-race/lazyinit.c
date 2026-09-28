// Several pieces of MLT state are initialised lazily without a lock, so the first
// producers opened on several threads at once can crash:
//   producer_loader.c: the static "dictionary" (create_producer) and "normalizers"
//                      (attach_normalizers);
//   mlt_service.c:     get_cache() creating the global "caches" properties;
//   avformat/factory.c: "avformat_initialised".
// Four threads each open a file through the default loader and read one frame, first
// thing after mlt_factory_init(). With "warm", one open on the main thread comes first.
// Actual (MLT 7.40.0): 31 of 200 runs segfault; with "warm", 0 of 200.
//
// Input:  ffmpeg -f lavfi -i testsrc2=s=640x360:r=25:d=2 -pix_fmt yuv420p clip.mp4
// Build:  cc -g lazyinit.c -o lazyinit -pthread $(pkg-config --cflags --libs mlt-framework-7)
// Run:    for i in $(seq 200); do ./lazyinit clip.mp4 || echo crash; done | grep -c crash
#include <framework/mlt.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

enum { THREADS = 4 }; // within mlt_cache's default of 4 avformat decoders
static mlt_profile profile;
static const char *resource;
static pthread_barrier_t barrier;

static void *open_one(void *arg)
{
    if (arg)
        pthread_barrier_wait(&barrier);
    mlt_producer p = mlt_factory_producer(profile, NULL, resource);
    if (p) {
        mlt_frame frame = NULL;
        mlt_service_get_frame(MLT_PRODUCER_SERVICE(p), &frame, 0);
        mlt_frame_close(frame);
        mlt_producer_close(p);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    resource = argc > 1 ? argv[1] : "clip.mp4";
    mlt_factory_init(NULL);
    profile = mlt_profile_init(NULL);
    if (argc > 2 && !strcmp(argv[2], "warm"))
        open_one(NULL); // one open and decode on this thread first: no crash
    pthread_barrier_init(&barrier, NULL, THREADS);
    pthread_t threads[THREADS];
    for (int i = 0; i < THREADS; i++)
        pthread_create(&threads[i], NULL, open_one, &barrier);
    for (int i = 0; i < THREADS; i++)
        pthread_join(threads[i], NULL);
    mlt_profile_close(profile);
    mlt_factory_close();
    return 0;
}
