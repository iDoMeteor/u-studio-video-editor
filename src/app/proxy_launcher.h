#pragma once

#include "proxy_queue.h"

namespace ustudio::app {

// ProxyQueue's launcher in the app: a GSubprocess with its stdout read line
// by line on the main loop, cancelled through platform::requestTermination()
// (ADR-017: SIGTERM on Linux). The child's stderr goes to ours, so its log
// lines land in the editor's log.
class GioLauncher : public ProxyQueue::Launcher
{
  public:
    std::unique_ptr<ProxyQueue::Child> start(const std::vector<std::string> &argv,
                                             std::function<void(const std::string &line)> onLine,
                                             std::function<void(int status)> onExit) override;
};

} // namespace ustudio::app
