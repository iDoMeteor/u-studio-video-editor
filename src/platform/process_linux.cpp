#include "platform/process.h"

#include <unistd.h>

namespace ustudio::platform {

int64_t currentProcessId()
{
    return static_cast<int64_t>(::getpid());
}

} // namespace ustudio::platform
