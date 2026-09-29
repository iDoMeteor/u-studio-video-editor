// movit.convert leaks the movit::Input of every MltInput it parks on a frame whose chain
// already exists: dispose_movit_effects() deletes the MltInput, but ~MltInput() leaves
// its movit::Input to an EffectChain that never took it (filter_movit_convert.cpp).
// Black plus two movit.rect'd colour tracks over movit.overlay, 320x180, 1,200 frames
// pulled as RGBA with a surfaceless EGL context current (Mesa; needs a GPU driver).
// Actual (MLT master 0926a75, movit 1.7.1, Mesa 26.1.4, Intel Iris Xe):
//   RSS +5.68 KB a frame after warm-up; LeakSanitizer: 6.6 MB in 30,026 allocations,
//   groups of 3,597 objects (3 inputs x 1,199 frames).
// With master.patch: RSS +0.00 KB a frame; LeakSanitizer: only Mesa's EGL init remains
//   (187 KB in 1,250 allocations).
//
// Build:  cc movitleak.c -o movitleak $(pkg-config --cflags --libs mlt-framework-7) -lEGL
//         (add -fsanitize=address for LeakSanitizer)
// Run:    ./movitleak
#define EGL_NO_X11
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <framework/mlt.h>
#include <stdio.h>
#include <string.h>

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

static mlt_producer colour(mlt_profile profile, const char *c, const char *rect)
{
    mlt_producer p = mlt_factory_producer(profile, NULL, c);
    mlt_filter f = mlt_factory_filter(profile, "movit.rect", NULL);
    mlt_properties_set(MLT_FILTER_PROPERTIES(f), "rect", rect);
    mlt_producer_attach(p, f);
    mlt_filter_close(f);
    return p;
}

int main(void)
{
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display
        = (PFNEGLGETPLATFORMDISPLAYEXTPROC) eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay dpy = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    eglInitialize(dpy, NULL, NULL);
    eglBindAPI(EGL_OPENGL_API);
    const EGLint attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attrs);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);

    mlt_factory_init(NULL);
    mlt_profile profile = mlt_profile_init(NULL);
    profile->width = 320;
    profile->height = 180;
    mlt_filter glsl = mlt_factory_filter(profile, "glsl.manager", NULL);
    mlt_events_fire(MLT_FILTER_PROPERTIES(glsl), "init glsl", mlt_event_data_none());
    if (!mlt_properties_get_int(MLT_FILTER_PROPERTIES(glsl), "glsl_supported")) {
        fprintf(stderr, "no movit\n");
        return 1;
    }
    mlt_tractor tractor = mlt_tractor_new();
    mlt_producer black = mlt_factory_producer(profile, NULL, "color:black");
    mlt_producer red = colour(profile, "color:red", "0 0 160 90");
    mlt_producer blue = colour(profile, "color:blue", "160 90 160 90");
    mlt_tractor_set_track(tractor, black, 0);
    mlt_tractor_set_track(tractor, red, 1);
    mlt_tractor_set_track(tractor, blue, 2);
    for (int track = 1; track <= 2; track++) {
        mlt_transition t = mlt_factory_transition(profile, "movit.overlay", NULL);
        mlt_field_plant_transition(mlt_tractor_field(tractor), t, 0, track);
        mlt_transition_close(t);
    }
    long warm = 0;
    for (int i = 0; i < 1200; i++) {
        mlt_frame frame = NULL;
        mlt_producer_seek(MLT_TRACTOR_PRODUCER(tractor), i % 100);
        mlt_service_get_frame(MLT_TRACTOR_SERVICE(tractor), &frame, 0);
        mlt_image_format format = mlt_image_rgba;
        int w = profile->width, h = profile->height;
        uint8_t *image = NULL;
        mlt_frame_get_image(frame, &image, &format, &w, &h, 0);
        if (i == 0)
            printf("frame 0: %dx%d, pixel (40,20) = %d,%d,%d; (240,135) = %d,%d,%d\n", w, h,
                   image[(20 * w + 40) * 4], image[(20 * w + 40) * 4 + 1], image[(20 * w + 40) * 4 + 2],
                   image[(135 * w + 240) * 4], image[(135 * w + 240) * 4 + 1], image[(135 * w + 240) * 4 + 2]);
        mlt_frame_close(frame);
        if (i == 199)
            warm = rss_kb();
    }
    printf("RSS growth after warm-up: %.2f KB a frame\n", (rss_kb() - warm) / 1000.0);
    mlt_tractor_close(tractor);
    mlt_producer_close(black);
    mlt_producer_close(red);
    mlt_producer_close(blue);
    mlt_events_fire(MLT_FILTER_PROPERTIES(glsl), "close glsl", mlt_event_data_none());
    mlt_filter_close(glsl);
    mlt_profile_close(profile);
    mlt_factory_close();
    return 0;
}
