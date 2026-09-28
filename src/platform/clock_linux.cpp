#include "platform/clock.h"

namespace ustudio::platform {

std::tm localTime(std::time_t when)
{
    std::tm local{};
    localtime_r(&when, &local);
    return local;
}

} // namespace ustudio::platform
