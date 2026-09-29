// consumer_sdl2_audio.c, consumer_thread(), paused path (speed 0):
//     } else if (self->running) {                        // checked without refresh_mutex
//         pthread_mutex_lock(&self->refresh_mutex);
//         consumer_play_video(self, frame);
//         ...
//         self->refresh_count--;
//         if (self->refresh_count <= 0)
//             pthread_cond_wait(&self->refresh_cond, ...); // running not checked again
// consumer_stop() sets running = 0 and broadcasts refresh_cond under refresh_mutex. If
// that broadcast lands after the check and before this thread locks refresh_mutex, it is
// lost, the thread waits for good, and consumer_stop()'s pthread_join() never returns.
// This is from reading the source. The window is a few instructions, and this stress
// test (paused stops) has NOT hung here: 1,200 stops, SDL_AUDIODRIVER=dummy.
//
// Input:  ffmpeg -f lavfi -i testsrc2=s=640x360:r=25:d=8 -pix_fmt yuv420p clip.mp4
// Build:  cc stopwake.c -o stopwake $(pkg-config --cflags --libs mlt-framework-7)
// Run:    SDL_AUDIODRIVER=dummy ./stopwake clip.mp4 400
#include <framework/mlt.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static void sleep_us(int us)
{
    struct timespec t = {us / 1000000, (us % 1000000) * 1000L};
    nanosleep(&t, NULL);
}

int main(int argc, char **argv)
{
    const char *resource = argc > 1 ? argv[1] : "clip.mp4";
    int rounds = argc > 2 ? atoi(argv[2]) : 500;
    mlt_factory_init(NULL);
    mlt_profile profile = mlt_profile_init(NULL);
    mlt_producer producer = mlt_factory_producer(profile, NULL, resource);
    for (int i = 0; i < rounds; i++) {
        mlt_consumer consumer = mlt_factory_consumer(profile, "sdl2_audio", NULL);
        mlt_consumer_connect(consumer, MLT_PRODUCER_SERVICE(producer));
        mlt_producer_set_speed(producer, 0);
        mlt_consumer_start(consumer);
        // Paused: each refresh shows one frame, then the consumer thread waits.
        for (int k = 0; k < 3; k++) {
            mlt_producer_seek(producer, rand() % 100);
            mlt_properties_set_int(MLT_CONSUMER_PROPERTIES(consumer), "refresh", 1);
            sleep_us(rand() % 5000);
        }
        mlt_consumer_stop(consumer); // hangs here when the wake-up is lost
        mlt_consumer_close(consumer);
        if (i % 50 == 0)
            fprintf(stderr, "round %d\n", i);
    }
    printf("%d stop() calls returned\n", rounds);
    mlt_producer_close(producer);
    mlt_profile_close(profile);
    mlt_factory_close();
    return 0;
}
