#include "export.h"

#include "core/log.h"
#include "core/media/utf8_path.h"
#include "platform/process.h"

#include <filesystem>
#include <memory>

namespace ustudio::titles::app {

namespace {
bool isFile(const std::filesystem::path &path)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

struct Pending
{
    std::function<void(const std::string &)> done;
};

// The tool's one JSON line: {"output":...} or {"error":"..."}.
std::string errorIn(const std::string &json)
{
    const std::string key = R"("error":")";
    const size_t at = json.find(key);
    if (at == std::string::npos)
        return json.find(R"("output")") == std::string::npos ? "the render tool said nothing" : "";
    std::string message;
    for (size_t i = at + key.size(); i < json.size() && json[i] != '"'; ++i) {
        if (json[i] == '\\' && i + 1 < json.size())
            ++i;
        message += json[i];
    }
    return message;
}

void onFinished(GObject *source, GAsyncResult *result, gpointer data)
{
    std::unique_ptr<Pending> pending(static_cast<Pending *>(data));
    char *output = nullptr;
    GError *error = nullptr;
    const gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &output, nullptr, &error);
    if (!ok && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_clear_error(&error);
        return; // the window is gone
    }
    std::string message;
    if (!ok)
        message = error ? error->message : "the render tool failed";
    else
        message = errorIn(output ? output : "");
    g_clear_error(&error);
    g_free(output);
    pending->done(message);
}
} // namespace

std::string renderToolPath()
{
    const std::string name = std::string("u-studio-render") + platform::executableSuffix();
    const std::filesystem::path self = platform::executablePath();
    if (!self.empty() && isFile(self.parent_path() / name))
        return core::utf8String(self.parent_path() / name);
    if (isFile(core::pathFromUtf8(TITLES_RENDER_BUILD_PATH)))
        return TITLES_RENDER_BUILD_PATH;
    char *found = g_find_program_in_path(name.c_str());
    const std::string out = found ? found : "";
    g_free(found);
    return out;
}

void exportTitle(const std::string &title, const std::string &output, const std::string &format, double seconds,
                 GCancellable *cancellable, std::function<void(const std::string &error)> done)
{
    const std::string tool = renderToolPath();
    if (tool.empty()) {
        done("the render tool (u-studio-render) isn't installed");
        return;
    }
    const std::string length = std::to_string(seconds);
    const char *argv[] = {tool.c_str(),   "--title-export", title.c_str(),  output.c_str(),
                          format.c_str(), "--seconds",      length.c_str(), nullptr};
    GSubprocessLauncher *launcher = g_subprocess_launcher_new(
        static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE));
#ifdef TITLES_DROPIN_MODULE_DIR
    if (tool == TITLES_RENDER_BUILD_PATH)
        g_subprocess_launcher_setenv(launcher, "USTUDIO_DROPIN_PATH", TITLES_DROPIN_MODULE_DIR, TRUE);
#endif
    GError *error = nullptr;
    GSubprocess *process = g_subprocess_launcher_spawnv(launcher, argv, &error);
    g_object_unref(launcher);
    if (!process) {
        done(error ? error->message : "the render tool didn't start");
        g_clear_error(&error);
        return;
    }
    core::Log::info("[titles] exporting " + title + " as " + format + " to " + output);
    g_subprocess_communicate_utf8_async(process, nullptr, cancellable, &onFinished, new Pending{std::move(done)});
    g_object_unref(process);
}

} // namespace ustudio::titles::app
