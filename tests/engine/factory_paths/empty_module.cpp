// Stands in for a drop-in's MLT module (IP4 test): MLT's repository calls a
// module's mlt_register() at Factory::init(); this one registers no
// services, only leaves a mark the test can see.
#include <cstdlib>

extern "C" __attribute__((visibility("default"))) void mlt_register(void *)
{
    ::setenv("USTUDIO_TEST_MODULE_REGISTERED", "1", 1);
}
