// A frei0r filter that is broken on purpose, for the health scan's tests
// (doc 15, "Testing"): built as crashy.so (a write through a null pointer
// in f0r_update) and, with -DBROKEN_HANG, hangy.so (f0r_update never
// returns). frei0r's header isn't installed on the dev machine, so the C
// ABI MLT's frei0r module looks up is declared here, from the frei0r 1.2
// specification (frei0r.h).

#include <cstdint>

extern "C" {

typedef void *f0r_instance_t;
typedef void *f0r_param_t;

typedef struct f0r_plugin_info
{
    const char *name;
    const char *author;
    int plugin_type; /* F0R_PLUGIN_TYPE_FILTER = 0 */
    int color_model; /* F0R_COLOR_MODEL_RGBA8888 = 1 */
    int frei0r_version;
    int major_version;
    int minor_version;
    int num_params;
    const char *explanation;
} f0r_plugin_info_t;

typedef struct f0r_param_info
{
    const char *name;
    int type;
    const char *explanation;
} f0r_param_info_t;

int f0r_init(void)
{
    return 1;
}

void f0r_deinit(void) {}

void f0r_get_plugin_info(f0r_plugin_info_t *info)
{
#ifdef BROKEN_HANG
    info->name = "hangy";
    info->explanation = "Never returns from f0r_update (tests only)";
#else
    info->name = "crashy";
    info->explanation = "Crashes in f0r_update (tests only)";
#endif
    info->author = "u Studio tests";
    info->plugin_type = 0;
    info->color_model = 1;
    info->frei0r_version = 1;
    info->major_version = 1;
    info->minor_version = 0;
    info->num_params = 0;
}

void f0r_get_param_info(f0r_param_info_t *info, int index)
{
    (void)info;
    (void)index;
}

static int theInstance;

f0r_instance_t f0r_construct(unsigned int width, unsigned int height)
{
    (void)width;
    (void)height;
    return &theInstance;
}

void f0r_destruct(f0r_instance_t instance)
{
    (void)instance;
}

void f0r_set_param_value(f0r_instance_t instance, f0r_param_t param, int index)
{
    (void)instance;
    (void)param;
    (void)index;
}

void f0r_get_param_value(f0r_instance_t instance, f0r_param_t param, int index)
{
    (void)instance;
    (void)param;
    (void)index;
}

void f0r_update(f0r_instance_t instance, double time, const uint32_t *in, uint32_t *out)
{
    (void)instance;
    (void)time;
    (void)in;
    (void)out;
#ifdef BROKEN_HANG
    for (volatile int spin = 1; spin;)
        continue;
#else
    volatile uint32_t *nowhere = nullptr;
    *nowhere = 1;
#endif
}

} // extern "C"
