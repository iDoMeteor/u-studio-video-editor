#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "app/portal_path.h"

#include <fcntl.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

using namespace ustudio::app;
namespace fs = std::filesystem;

namespace {

// Registers `file` with the real xdg-document-portal (Documents.Add, the
// same call the file chooser makes for an existing file) and returns its
// FUSE path, or nullopt when there's no session bus or portal (CI
// containers) -- the caller skips then. Registered non-persistent, so the
// entry goes away with the session. Documents.Delete is refused to
// unprivileged callers, so it can't be removed sooner.
std::optional<std::string> exportThroughPortal(const std::string &file)
{
    GError *error = nullptr;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!bus) {
        g_clear_error(&error);
        return std::nullopt;
    }
    int fd = open(file.c_str(), O_RDWR | O_CLOEXEC);
    GUnixFDList *fds = g_unix_fd_list_new();
    int index = g_unix_fd_list_append(fds, fd, nullptr);
    close(fd);
    GVariant *reply = g_dbus_connection_call_with_unix_fd_list_sync(
        bus, "org.freedesktop.portal.Documents", "/org/freedesktop/portal/documents",
        "org.freedesktop.portal.Documents", "Add", g_variant_new("(hbb)", index, TRUE, FALSE), G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE, 5000, fds, nullptr, nullptr, &error);
    g_object_unref(fds);
    g_object_unref(bus);
    if (!reply) {
        MESSAGE("Documents portal unavailable: " << (error ? error->message : "?"));
        g_clear_error(&error);
        return std::nullopt;
    }
    const char *docId = nullptr;
    g_variant_get(reply, "(&s)", &docId);
    std::string portalPath =
        std::string(g_get_user_runtime_dir()) + "/doc/" + docId + "/" + fs::path(file).filename().string();
    g_variant_unref(reply);
    return portalPath;
}

} // namespace

TEST_CASE("portal::resolveHostPath leaves ordinary paths alone")
{
    CHECK(portal::resolveHostPath("/home/someone/project.ustudio") == "/home/someone/project.ustudio");
    CHECK(portal::resolveHostPath("relative/project.ustudio") == "relative/project.ustudio");
}

TEST_CASE("portal::resolveHostPath maps a document-portal path back to the real file")
{
    // Under the build dir (tests/app/meson.build sets it as the working
    // directory), a real filesystem.
    fs::path dir = fs::absolute("portal-path-test");
    fs::create_directories(dir);
    std::string host = (dir / "project.ustudio").string();
    std::ofstream(host) << "original";

    auto portalPath = exportThroughPortal(host);
    if (!portalPath) {
        MESSAGE("skipping: no Documents portal in this environment");
        return;
    }
    REQUIRE(fs::exists(*portalPath));
    CHECK(*portalPath != host);

    std::string resolved = portal::resolveHostPath(*portalPath);
    CHECK(resolved == host);

    // The point of resolving: the atomic save's temp sibling can be created
    // next to the resolved path (it's the real directory).
    std::string tmp = resolved + ".tmp";
    {
        std::ofstream out(tmp);
        CHECK(out.good());
    }
    fs::remove(tmp);
}
