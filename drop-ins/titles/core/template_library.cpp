#include "template_library.h"

#include "title_xml.h"

#include "core/media/utf8_path.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <set>
#include <tuple>

namespace ustudio::titles {

namespace fs = std::filesystem;

namespace {

constexpr const char *kTemplateFile = "template.ustitle";
constexpr const char *kPreviewFile = "preview.png";

std::optional<TemplateInfo> infoFor(const fs::path &file, bool builtIn)
{
    auto read = readTitle(core::utf8String(file));
    if (!read)
        return std::nullopt;
    TemplateInfo info;
    info.builtIn = builtIn;
    info.path = core::utf8String(file);
    info.id = core::utf8String(builtIn ? file.stem() : file.parent_path().filename());
    info.name = read->document.name.empty() ? info.id : read->document.name;
    info.category = read->document.category;
    const fs::path preview = builtIn ? fs::path(file).replace_extension(".png") : file.parent_path() / kPreviewFile;
    std::error_code ec;
    if (fs::is_regular_file(preview, ec))
        info.preview = core::utf8String(preview);
    if (!builtIn)
        info.folder = core::utf8String(file.parent_path());
    return info;
}

// A name in `dir` that isn't taken: `base`, then `base-2`, `base-3`, ...
fs::path freeName(const fs::path &dir, const std::string &base, const std::string &extension = "")
{
    std::error_code ec;
    for (int n = 1;; ++n) {
        const fs::path candidate = dir / core::pathFromUtf8(base + (n == 1 ? "" : "-" + std::to_string(n)) + extension);
        if (!fs::exists(candidate, ec))
            return candidate;
    }
}

// Copies every picture `doc` uses into `imagesDir` (made when needed) and
// points the layers at them as `prefix` + file name. A picture that isn't
// there is left as it was: the title warns about it as before.
std::expected<void, std::string> takePictures(TitleDocument &doc, const fs::path &imagesDir, const std::string &prefix)
{
    std::set<std::string> used;
    for (Layer &layer : doc.layers) {
        if (layer.kind != LayerKind::Image || layer.src.empty())
            continue;
        fs::path source = core::pathFromUtf8(layer.src);
        if (source.is_relative() && !doc.baseDirectory.empty())
            source = core::pathFromUtf8(doc.baseDirectory) / source;
        std::error_code ec;
        if (!fs::is_regular_file(source, ec))
            continue;
        fs::create_directories(imagesDir, ec);
        if (ec)
            return std::unexpected("can't make " + core::utf8String(imagesDir) + " (" + ec.message() + ")");
        // The same file used twice is copied once; two files with one name
        // get distinct names.
        std::string name = core::utf8String(source.filename());
        const fs::path target = imagesDir / core::pathFromUtf8(name);
        if (!used.contains(core::utf8String(source))) {
            fs::path free = target;
            if (fs::exists(free, ec))
                free = freeName(imagesDir, core::utf8String(source.stem()), core::utf8String(source.extension()));
            fs::copy_file(source, free, ec);
            if (ec)
                return std::unexpected("can't copy " + core::utf8String(source) + " (" + ec.message() + ")");
            name = core::utf8String(free.filename());
            used.insert(core::utf8String(source));
        }
        layer.src = prefix + name;
    }
    return {};
}

} // namespace

std::string templateSlug(const std::string &name)
{
    std::string slug;
    for (unsigned char c : name) {
        if (std::isalnum(c) && c < 0x80)
            slug += static_cast<char>(std::tolower(c));
        else if (!slug.empty() && slug.back() != '-')
            slug += '-';
    }
    while (!slug.empty() && slug.back() == '-')
        slug.pop_back();
    return slug.empty() ? "template" : slug;
}

std::vector<TemplateInfo> listTemplates(const std::string &dir, bool builtIn)
{
    std::vector<TemplateInfo> out;
    std::error_code ec;
    const fs::path root = core::pathFromUtf8(dir);
    if (!fs::is_directory(root, ec))
        return out;
    for (const fs::directory_entry &entry : fs::directory_iterator(root, ec)) {
        std::optional<TemplateInfo> info;
        if (builtIn && entry.is_regular_file(ec) && isTitleFile(core::utf8String(entry.path())))
            info = infoFor(entry.path(), true);
        else if (!builtIn && entry.is_directory(ec) && fs::is_regular_file(entry.path() / kTemplateFile, ec))
            info = infoFor(entry.path() / kTemplateFile, false);
        if (info)
            out.push_back(std::move(*info));
    }
    std::sort(out.begin(), out.end(), [](const TemplateInfo &a, const TemplateInfo &b) {
        return std::tie(a.category, a.name, a.id) < std::tie(b.category, b.name, b.id);
    });
    return out;
}

std::expected<TemplateInfo, std::string> saveTemplate(const TitleDocument &doc, const std::string &library,
                                                      const std::string &name)
{
    if (name.empty())
        return std::unexpected("a template needs a name");
    const fs::path root = core::pathFromUtf8(library);
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec)
        return std::unexpected("can't make " + library + " (" + ec.message() + ")");
    const fs::path folder = freeName(root, templateSlug(name));
    fs::path part = folder;
    part += ".part";
    fs::remove_all(part, ec);
    fs::create_directories(part, ec);
    if (ec)
        return std::unexpected("can't make " + core::utf8String(part) + " (" + ec.message() + ")");
    TitleDocument copy = doc;
    copy.name = name;
    if (auto taken = takePictures(copy, part / "images", "images/"); !taken) {
        fs::remove_all(part, ec);
        return std::unexpected(taken.error());
    }
    if (const std::string error = saveTitle(copy, core::utf8String(part / kTemplateFile)); !error.empty()) {
        fs::remove_all(part, ec);
        return std::unexpected(error);
    }
    fs::rename(part, folder, ec);
    if (ec) {
        fs::remove_all(part, ec);
        return std::unexpected("can't save the template (" + ec.message() + ")");
    }
    auto info = infoFor(folder / kTemplateFile, false);
    if (!info)
        return std::unexpected("the saved template doesn't read back");
    return *info;
}

std::expected<TemplateInfo, std::string> renameTemplate(const TemplateInfo &info, const std::string &name)
{
    if (info.builtIn)
        return std::unexpected("a built-in template can't be renamed; duplicate it first");
    if (name.empty())
        return std::unexpected("a template needs a name");
    auto read = readTitle(info.path);
    if (!read)
        return std::unexpected(read.error());
    read->document.name = name;
    if (const std::string error = saveTitle(read->document, info.path); !error.empty())
        return std::unexpected(error);
    auto renamed = infoFor(core::pathFromUtf8(info.path), false);
    if (!renamed)
        return std::unexpected("the renamed template doesn't read back");
    return *renamed;
}

std::expected<TemplateInfo, std::string> duplicateTemplate(const TemplateInfo &info, const std::string &library,
                                                           const std::string &name)
{
    auto read = readTitle(info.path);
    if (!read)
        return std::unexpected(read.error());
    read->document.category = info.builtIn ? std::string() : read->document.category;
    return saveTemplate(read->document, library, name);
}

std::expected<void, std::string> removeTemplate(const TemplateInfo &info)
{
    if (info.builtIn || info.folder.empty())
        return std::unexpected("a built-in template can't be deleted");
    // Only a folder that is a template: never whatever a stray path names.
    const fs::path folder = core::pathFromUtf8(info.folder);
    std::error_code ec;
    if (!fs::is_regular_file(folder / kTemplateFile, ec))
        return std::unexpected(info.folder + " isn't a template");
    fs::remove_all(folder, ec);
    if (ec)
        return std::unexpected("can't delete " + info.folder + " (" + ec.message() + ")");
    return {};
}

std::expected<TitleDocument, std::string> templateDocument(const TemplateInfo &info, const std::string &titlePath)
{
    auto read = readTitle(info.path);
    if (!read)
        return std::unexpected(read.error());
    TitleDocument doc = std::move(read->document);
    doc.name.clear();
    doc.category.clear();
    if (titlePath.empty()) {
        for (Layer &layer : doc.layers)
            if (layer.kind == LayerKind::Image && !layer.src.empty() && core::pathFromUtf8(layer.src).is_relative())
                layer.src = core::utf8String(core::pathFromUtf8(doc.baseDirectory) / core::pathFromUtf8(layer.src));
        return doc;
    }
    const fs::path target = core::pathFromUtf8(titlePath);
    // A pack's template: its fonts go beside the title (fonts/), where the
    // editor's producer and the designer look for a title's own fonts.
    std::error_code ec;
    const fs::path packFolder = core::pathFromUtf8(info.folder).parent_path();
    if (!info.folder.empty() && fs::is_regular_file(packFolder / "pack.xml", ec) &&
        fs::is_directory(packFolder / "fonts", ec)) {
        fs::create_directories(target.parent_path() / "fonts", ec);
        for (const fs::directory_entry &font : fs::directory_iterator(packFolder / "fonts", ec))
            if (font.is_regular_file(ec) && !fs::exists(target.parent_path() / "fonts" / font.path().filename(), ec))
                fs::copy_file(font.path(), target.parent_path() / "fonts" / font.path().filename(), ec);
    }
    const std::string images = core::utf8String(target.stem()) + " images";
    if (auto taken = takePictures(doc, target.parent_path() / core::pathFromUtf8(images), images + "/"); !taken)
        return std::unexpected(taken.error());
    doc.baseDirectory = core::utf8String(target.parent_path());
    return doc;
}

std::expected<void, std::string> newTitleFromTemplate(const TemplateInfo &info, const std::string &destination)
{
    std::error_code ec;
    if (fs::exists(core::pathFromUtf8(destination), ec))
        return std::unexpected(destination + " already exists");
    auto doc = templateDocument(info, destination);
    if (!doc)
        return std::unexpected(doc.error());
    if (const std::string error = saveTitle(*doc, destination); !error.empty())
        return std::unexpected(error);
    return {};
}

} // namespace ustudio::titles
