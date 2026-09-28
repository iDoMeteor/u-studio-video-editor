// composite's halign/valign are right only when the frame is read at the profile's size.
// A 4:3 picture composited with fill=1 halign=centre valign=middle in a 1080p profile:
// read at 1920x1080 it is centred; read at 960x540 it is shifted right.
// Expected: equal left and right margins at both sizes (240/240 and 120/120).
// Actual (MLT 7.40.0): 240/240 at 1920x1080, 300/0 at 960x540.
// transition_composite.c's get_image aligns item.w (profile pixels) against the B
// image's width (requested pixels) in the second alignment_calculate() call.
//
// Input:  ffmpeg -f lavfi -i color=c=red:s=640x480:r=25:d=1 -pix_fmt yuv420p red43.mp4
// Build:  cc align.c -o align $(pkg-config --cflags --libs mlt-framework-7)
// Run:    ./align red43.mp4
#include <framework/mlt.h>
#include <stdio.h>
int main(int argc, char **argv)
{
    if (argc < 2)
        return 1;
    mlt_factory_init(NULL);
    mlt_profile profile = mlt_profile_init("atsc_1080p_25");
    mlt_tractor tractor = mlt_tractor_new();
    mlt_producer bg = mlt_factory_producer(profile, NULL, "color:black");
    mlt_producer fg = mlt_factory_producer(profile, NULL, argv[1]);
    mlt_tractor_set_track(tractor, bg, 0);
    mlt_tractor_set_track(tractor, fg, 1);
    mlt_transition t = mlt_factory_transition(profile, "composite", NULL);
    mlt_properties_set(MLT_TRANSITION_PROPERTIES(t), "fill", "1");
    mlt_properties_set(MLT_TRANSITION_PROPERTIES(t), "halign", "centre");
    mlt_properties_set(MLT_TRANSITION_PROPERTIES(t), "valign", "middle");
    mlt_field_plant_transition(mlt_tractor_field(tractor), t, 0, 1);
    int sizes[][2] = {{1920, 1080}, {960, 540}};
    for (int i = 0; i < 2; i++) {
        mlt_frame frame = NULL;
        mlt_producer_seek(MLT_TRACTOR_PRODUCER(tractor), 0);
        mlt_service_get_frame(MLT_TRACTOR_SERVICE(tractor), &frame, 0);
        mlt_image_format fmt = mlt_image_rgba;
        int w = sizes[i][0], h = sizes[i][1];
        uint8_t *img = NULL;
        mlt_frame_get_image(frame, &img, &fmt, &w, &h, 0);
        uint8_t *row = img + (h / 2) * w * 4;
        int first = -1, last = -1;
        for (int x = 0; x < w; x++)
            if (row[4 * x] > 128 && row[4 * x + 1] < 100) {
                if (first < 0) first = x;
                last = x;
            }
        printf("requested %dx%d, got %dx%d: red spans x=%d..%d, left margin %d, right margin %d\n",
               sizes[i][0], sizes[i][1], w, h, first, last, first, w - 1 - last);
        mlt_frame_close(frame);
    }
    mlt_transition_close(t);
    mlt_producer_close(fg);
    mlt_producer_close(bg);
    mlt_tractor_close(tractor);
    mlt_profile_close(profile);
    mlt_factory_close();
    return 0;
}
