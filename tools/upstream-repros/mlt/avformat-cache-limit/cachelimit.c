// mlt_cache keeps the decoder state of at most 4 avformat producers process-wide
// ("producer_avformat", mlt_service_cache_put() in producer_avformat.c) and evicts the
// least recently used when another one decodes, even while its own thread is still
// decoding with it. Sixteen threads, each with its own producer, read video and audio.
// Actual (MLT 7.40.0, 16 cores): 30 of 30 runs segfault, in producer_get_frame() ->
// mlt_properties_get(); with "raise" (cache size 17), 0 of 30.
//
// Input:  ffmpeg -f lavfi -i testsrc2=s=640x360:r=25:d=8 -f lavfi -i sine=d=8
//             -c:v libx264 -g 25 -pix_fmt yuv420p -c:a aac clip.mp4
// Build:  cc -g cachelimit.c -o cachelimit -pthread $(pkg-config --cflags --libs mlt-framework-7)
// Run:    for i in $(seq 30); do ./cachelimit clip.mp4 || echo crash; done | grep -c crash
//         for i in $(seq 30); do ./cachelimit clip.mp4 raise || echo crash; done | grep -c crash
#include <framework/mlt.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

enum { THREADS = 16, FRAMES = 150 };
static mlt_profile profile;
static const char *resource;

static void *decode(void *arg)
{
    mlt_producer p = arg;
    for (int i = 0; i < FRAMES; i++) {
        mlt_frame frame = NULL;
        mlt_producer_seek(p, i);
        mlt_service_get_frame(MLT_PRODUCER_SERVICE(p), &frame, 0);
        mlt_image_format fmt = mlt_image_yuv422;
        int w = 0, h = 0;
        uint8_t *image = NULL;
        mlt_frame_get_image(frame, &image, &fmt, &w, &h, 0);
        mlt_audio_format afmt = mlt_audio_s16;
        int freq = 48000, channels = 2, samples = mlt_audio_calculate_frame_samples(25, freq, i);
        void *audio = NULL;
        mlt_frame_get_audio(frame, &audio, &afmt, &freq, &channels, &samples);
        mlt_frame_close(frame);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    resource = argc > 1 ? argv[1] : "clip.mp4";
    mlt_factory_init(NULL);
    profile = mlt_profile_init(NULL);
    // Open everything on this thread, so the lazy-init races (a separate issue) can't hit.
    mlt_producer producers[THREADS];
    for (int i = 0; i < THREADS; i++)
        producers[i] = mlt_factory_producer(profile, "avformat", resource);
    if (argc > 2 && !strcmp(argv[2], "raise")) // room for every decoder: no crash
        mlt_service_cache_set_size(MLT_PRODUCER_SERVICE(producers[0]), "producer_avformat", THREADS + 1);
    pthread_t threads[THREADS];
    for (int i = 0; i < THREADS; i++)
        pthread_create(&threads[i], NULL, decode, producers[i]);
    for (int i = 0; i < THREADS; i++)
        pthread_join(threads[i], NULL);
    for (int i = 0; i < THREADS; i++)
        mlt_producer_close(producers[i]);
    mlt_profile_close(profile);
    mlt_factory_close();
    printf("done\n");
    return 0;
}
