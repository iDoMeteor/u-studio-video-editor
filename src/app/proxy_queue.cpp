#include "proxy_queue.h"

#include "core/log.h"
#include "core/media/utf8_path.h"
#include "platform/process.h"

#include <glib.h>

#include <cstdlib>
#include <filesystem>

namespace ustudio::app {

namespace Log = ustudio::core::Log;

namespace {

std::string rate(core::Rational fps)
{
    return std::to_string(fps.num) + "/" + std::to_string(fps.den);
}

} // namespace

std::optional<std::string> jsonField(const std::string &line, const std::string &key)
{
    const std::string needle = "\"" + key + "\":";
    size_t pos = line.find(needle);
    if (pos == std::string::npos)
        return std::nullopt;
    pos += needle.size();
    if (pos >= line.size())
        return std::nullopt;
    if (line[pos] != '"') {
        const size_t end = line.find_first_of(",}", pos);
        return line.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    }
    std::string value;
    for (++pos; pos < line.size() && line[pos] != '"'; ++pos) {
        if (line[pos] == '\\' && pos + 1 < line.size()) {
            ++pos;
            if (line[pos] == 'u' && pos + 4 < line.size()) {
                value += static_cast<char>(std::strtol(line.substr(pos + 1, 4).c_str(), nullptr, 16));
                pos += 4;
                continue;
            }
        }
        value += line[pos];
    }
    return value;
}

ProxyQueue::ProxyQueue(std::unique_ptr<Launcher> launcher, std::string toolPath, Callbacks callbacks, size_t concurrent)
    : m_launcher(std::move(launcher)), m_toolPath(std::move(toolPath)), m_callbacks(std::move(callbacks)),
      m_concurrent(std::max<size_t>(concurrent, 1))
{}

std::vector<std::string> ProxyQueue::commandFor(const Job &job) const
{
    std::vector<std::string> argv{m_toolPath, "--proxy", job.source,   job.output,
                                  "--height",  std::to_string(job.height), "--fps", rate(job.fps)};
    if (job.sequenceCount > 0) {
        argv.push_back("--sequence");
        argv.push_back(std::to_string(job.sequenceBegin) + ":" + std::to_string(job.sequenceCount));
    }
    return argv;
}

void ProxyQueue::add(Job job)
{
    if (m_running.contains(job.asset.value))
        return;
    for (const Job &waiting : m_waiting)
        if (waiting.asset == job.asset)
            return;
    m_waiting.push_back(std::move(job));
    startNext();
}

void ProxyQueue::startNext()
{
    while (m_running.size() < m_concurrent && !m_waiting.empty()) {
        Job job = std::move(m_waiting.front());
        m_waiting.pop_front();
        const core::AssetId asset = job.asset;
        const std::vector<std::string> argv = commandFor(job);
        Running &running = m_running[asset.value];
        running.job = std::move(job);
        Log::info("[proxy] starting " + running.job.source + " -> " + running.job.output);
        // start() may call onExit at once (it couldn't start), which erases
        // `running`: take the child only if the entry is still there.
        std::unique_ptr<Child> child = m_launcher->start(
            argv, [this, asset](const std::string &line) { onLine(asset, line); },
            [this, asset](int status) { onExit(asset, status); });
        if (auto it = m_running.find(asset.value); it != m_running.end())
            it->second.child = std::move(child);
    }
}

void ProxyQueue::onLine(core::AssetId asset, const std::string &line)
{
    auto it = m_running.find(asset.value);
    if (it == m_running.end())
        return;
    Running &running = it->second;
    if (auto progress = jsonField(line, "progress")) {
        running.progress = std::strtod(progress->c_str(), nullptr);
        if (m_callbacks.progress)
            m_callbacks.progress(asset, running.progress);
    } else if (auto status = jsonField(line, "status")) {
        running.ok = *status == "ok";
        if (!running.ok)
            running.error = jsonField(line, "message").value_or("the proxy couldn't be made");
    }
}

void ProxyQueue::onExit(core::AssetId asset, int status)
{
    auto it = m_running.find(asset.value);
    if (it == m_running.end())
        return;
    Running finished = std::move(it->second);
    m_running.erase(it);
    if (finished.cancelled) {
        Log::info("[proxy] cancelled " + finished.job.source);
    } else if (status == 0 && finished.ok) {
        if (m_callbacks.done)
            m_callbacks.done(asset, finished.job.output);
    } else {
        const std::string message = !finished.error.empty() ? finished.error
                                    : status < 0 ? "u-studio-render couldn't be started"
                                                 : "u-studio-render stopped (status " + std::to_string(status) + ")";
        Log::warn("[proxy] " + finished.job.source + ": " + message);
        if (m_callbacks.failed)
            m_callbacks.failed(asset, message);
    }
    startNext();
}

void ProxyQueue::cancel(core::AssetId asset)
{
    std::erase_if(m_waiting, [&](const Job &job) { return job.asset == asset; });
    if (auto it = m_running.find(asset.value); it != m_running.end()) {
        it->second.cancelled = true;
        if (it->second.child)
            it->second.child->cancel(); // its exit arrives later
    }
}

void ProxyQueue::cancelAll()
{
    m_waiting.clear();
    for (auto &[id, running] : m_running) {
        running.cancelled = true;
        if (running.child)
            running.child->cancel();
    }
}

std::optional<double> ProxyQueue::progressOf(core::AssetId asset) const
{
    if (auto it = m_running.find(asset.value); it != m_running.end())
        return it->second.progress;
    for (const Job &job : m_waiting)
        if (job.asset == asset)
            return 0.0;
    return std::nullopt;
}

std::string locateRenderTool()
{
    namespace fs = std::filesystem;
    if (const char *overridden = std::getenv("USTUDIO_RENDER_BIN"); overridden && *overridden)
        return overridden;
    const fs::path exe = platform::executablePath();
    if (exe.empty())
        return {};
    const std::string name = std::string("u-studio-render") + platform::executableSuffix();
    const fs::path dir = exe.parent_path();
    // Installed: the same bindir. Build tree: src/app (the editor) or
    // tests/dropins (the test editor), two levels under builddir.
    for (const fs::path &candidate :
         {dir / name, dir / ".." / "render" / name, dir / ".." / ".." / "src" / "render" / name}) {
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec))
            return core::utf8String(fs::weakly_canonical(candidate, ec));
    }
    return {};
}

std::string proxyPathFor(const std::string &fingerprint, const std::string &sourcePath, int height)
{
    const std::string identity = (fingerprint.empty() ? sourcePath : fingerprint + "|" + sourcePath);
    gchar *hash =
        g_compute_checksum_for_string(G_CHECKSUM_SHA1, identity.c_str(), static_cast<gssize>(identity.size()));
    const std::string name =
        std::string(hash ? hash : "proxy") + "-" + (height > 0 ? std::to_string(height) : "src") + ".mp4";
    g_free(hash);
    const std::filesystem::path dir = core::pathFromUtf8(g_get_user_cache_dir()) / "ustudio" / "proxies";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return core::utf8String(dir / name);
}

} // namespace ustudio::app
