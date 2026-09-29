#include "platform/process.h"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <malloc.h>
#include <signal.h>
#include <sstream>
#include <string>
#include <unistd.h>

namespace ustudio::platform {

int64_t currentProcessId()
{
    return static_cast<int64_t>(::getpid());
}

std::filesystem::path executablePath()
{
    std::error_code ec;
    std::filesystem::path path = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path() : path;
}

const char *executableSuffix()
{
    return "";
}

namespace {
std::atomic<bool> *s_terminationFlag = nullptr;

void onTerminationSignal(int)
{
    // Async-signal-safe: a lock-free atomic store and nothing else.
    if (s_terminationFlag)
        s_terminationFlag->store(true);
}
} // namespace

void onTerminationRequest(std::atomic<bool> *flag)
{
    s_terminationFlag = flag;
    struct sigaction action = {};
    action.sa_handler = &onTerminationSignal;
    sigemptyset(&action.sa_mask);
    ::sigaction(SIGTERM, &action, nullptr);
    ::sigaction(SIGINT, &action, nullptr);
}

bool requestTermination(int64_t pid)
{
    return pid > 0 && ::kill(static_cast<pid_t>(pid), SIGTERM) == 0;
}

// kill(pid, 0) sends no signal, it only probes (POSIX kill(2)): ESRCH is
// no such process; EPERM is one owned by someone else, which exists.
bool processExists(int64_t pid)
{
    if (pid <= 0)
        return false;
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
}

// Field 22 (starttime, clock ticks since boot) of /proc/<pid>/stat.
int64_t processStartTime(int64_t pid)
{
    if (pid <= 0)
        return 0;
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    if (!in)
        return 0;
    std::string line;
    if (!std::getline(in, line))
        return 0;

    // comm (field 2) is "(...)"; and can itself contain spaces or even
    // parentheses, so the only robust split point is the LAST ')' on the
    // line, not the first space -- verified against /proc/self/stat's
    // own real field layout (2026-09-23).
    size_t closeParen = line.rfind(')');
    if (closeParen == std::string::npos || closeParen + 1 >= line.size())
        return 0;

    std::istringstream rest(line.substr(closeParen + 1));
    std::string token;
    int tokenIndex = 0; // field 3 (state) is rest's 1st token, so field 22 is its 20th
    while (rest >> token) {
        if (++tokenIndex == 20)
            return std::strtoll(token.c_str(), nullptr, 10);
    }
    return 0;
}

bool runningInFlatpak()
{
    // Flatpak puts this file at the sandbox's root (flatpak-metadata(5)).
    std::error_code ec;
    return std::filesystem::exists("/.flatpak-info", ec);
}

void releaseFreeMemory()
{
    malloc_trim(0);
}

} // namespace ustudio::platform
