#include "archive.h"

#include "core/media/utf8_path.h"

#include <cctype>
#include <filesystem>
#include <memory>

#ifdef TITLES_HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

namespace ustudio::titles::pack {

namespace fs = std::filesystem;

#ifdef TITLES_HAVE_LIBARCHIVE

namespace {

struct ReadFree
{
    void operator()(archive *a) const
    {
        archive_read_free(a);
    }
};
struct WriteFree
{
    void operator()(archive *a) const
    {
        archive_write_free(a);
    }
};
struct EntryFree
{
    void operator()(archive_entry *e) const
    {
        archive_entry_free(e);
    }
};

std::string errorOf(archive *a)
{
    const char *message = archive_error_string(a);
    return message ? message : "it isn't a pack archive";
}

bool endsWith(const std::string &text, std::string_view suffix)
{
    if (text.size() < suffix.size())
        return false;
    for (size_t i = 0; i < suffix.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(text[text.size() - suffix.size() + i])) != suffix[i])
            return false;
    return true;
}

} // namespace

bool archivesSupported()
{
    return true;
}

std::expected<std::vector<Entry>, std::string> readArchive(const std::string &path, const Limits &limits)
{
    std::unique_ptr<archive, ReadFree> reader(archive_read_new());
    // Only what a pack may be: zip, or tar (plain or gzip). No other
    // formats or filters, so no other decoders see the bytes.
    archive_read_support_format_zip(reader.get());
    archive_read_support_format_tar(reader.get());
    archive_read_support_format_gnutar(reader.get());
    archive_read_support_filter_gzip(reader.get());
    if (archive_read_open_filename(reader.get(), path.c_str(), 64 * 1024) != ARCHIVE_OK)
        return std::unexpected("can't open " + path + " (" + errorOf(reader.get()) + ")");
    std::vector<Entry> entries;
    uint64_t total = 0;
    archive_entry *header = nullptr;
    while (true) {
        const int status = archive_read_next_header(reader.get(), &header);
        if (status == ARCHIVE_EOF)
            break;
        if (status != ARCHIVE_OK && status != ARCHIVE_WARN)
            return std::unexpected("the pack is damaged (" + errorOf(reader.get()) + ")");
        if (entries.size() >= limits.maxFiles * 2) // directories count too, but not without end
            return std::unexpected("the pack has more than " + std::to_string(limits.maxFiles) + " files");
        Entry entry;
        const char *name = archive_entry_pathname(header);
        entry.path = name ? name : "";
        const auto type = archive_entry_filetype(header);
        if (archive_entry_hardlink(header) || archive_entry_symlink(header) || type == AE_IFLNK)
            entry.kind = Entry::Kind::Link;
        else if (type == AE_IFDIR)
            entry.kind = Entry::Kind::Directory;
        else if (type == AE_IFREG)
            entry.kind = Entry::Kind::File;
        else
            entry.kind = Entry::Kind::Other;
        if (entry.kind == Entry::Kind::File) {
            // Read as it comes, never trusting the declared size: stop at the
            // limits however much the entry claims or holds.
            char buffer[64 * 1024];
            while (true) {
                const la_ssize_t got = archive_read_data(reader.get(), buffer, sizeof buffer);
                if (got < 0)
                    return std::unexpected("the pack is damaged (" + errorOf(reader.get()) + ")");
                if (got == 0)
                    break;
                entry.data.append(buffer, static_cast<size_t>(got));
                total += static_cast<uint64_t>(got);
                if (entry.data.size() > limits.maxFileBytes)
                    return std::unexpected(entry.path + " is too big");
                if (total > limits.maxTotalBytes)
                    return std::unexpected("the pack unpacks to more than " +
                                           std::to_string(limits.maxTotalBytes >> 20) + " MB");
            }
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

std::expected<void, std::string> writeArchive(const std::vector<Entry> &entries, const std::string &path)
{
    const bool tarGz = endsWith(path, ".tar.gz") || endsWith(path, ".tgz");
    fs::path part = core::pathFromUtf8(path);
    part += ".part";
    std::error_code ec;
    fs::remove(part, ec);
    {
        std::unique_ptr<archive, WriteFree> writer(archive_write_new());
        if (tarGz) {
            archive_write_set_format_pax_restricted(writer.get());
            archive_write_add_filter_gzip(writer.get());
        } else {
            archive_write_set_format_zip(writer.get());
        }
        const std::string partPath = core::utf8String(part);
        if (archive_write_open_filename(writer.get(), partPath.c_str()) != ARCHIVE_OK)
            return std::unexpected("can't write " + path + " (" + errorOf(writer.get()) + ")");
        for (const Entry &entry : entries) {
            if (entry.kind != Entry::Kind::File)
                continue; // folders are implied by the paths
            std::unique_ptr<archive_entry, EntryFree> header(archive_entry_new());
            archive_entry_set_pathname(header.get(), entry.path.c_str());
            archive_entry_set_size(header.get(), static_cast<la_int64_t>(entry.data.size()));
            archive_entry_set_filetype(header.get(), AE_IFREG);
            archive_entry_set_perm(header.get(), 0644);
            if (archive_write_header(writer.get(), header.get()) != ARCHIVE_OK ||
                archive_write_data(writer.get(), entry.data.data(), entry.data.size()) !=
                    static_cast<la_ssize_t>(entry.data.size())) {
                const std::string why = errorOf(writer.get());
                writer.reset();
                fs::remove(part, ec);
                return std::unexpected("can't write " + path + " (" + why + ")");
            }
        }
        if (archive_write_close(writer.get()) != ARCHIVE_OK) {
            const std::string why = errorOf(writer.get());
            writer.reset();
            fs::remove(part, ec);
            return std::unexpected("can't write " + path + " (" + why + ")");
        }
    }
    fs::rename(part, core::pathFromUtf8(path), ec);
    if (ec) {
        fs::remove(part, ec);
        return std::unexpected("can't save " + path + " (" + ec.message() + ")");
    }
    return {};
}

#else

bool archivesSupported()
{
    return false;
}

std::expected<std::vector<Entry>, std::string> readArchive(const std::string &, const Limits &)
{
    return std::unexpected("this build can't open packs (it was built without libarchive)");
}

std::expected<void, std::string> writeArchive(const std::vector<Entry> &, const std::string &)
{
    return std::unexpected("this build can't save packs (it was built without libarchive)");
}

#endif

std::expected<ValidPack, std::string> inspectPackage(const std::string &path)
{
    std::error_code ec;
    const uint64_t size = std::filesystem::file_size(core::pathFromUtf8(path), ec);
    if (ec)
        return std::unexpected("can't read " + path + " (" + ec.message() + ")");
    auto entries = readArchive(path);
    if (!entries)
        return std::unexpected(entries.error());
    return validate(*entries, size);
}

std::expected<InstalledPack, std::string> openPackage(const std::string &path, const std::string &library,
                                                      Replace replace)
{
    auto pack = inspectPackage(path);
    if (!pack)
        return std::unexpected(pack.error());
    return install(*pack, library, replace);
}

} // namespace ustudio::titles::pack
