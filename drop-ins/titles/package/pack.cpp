#include "pack.h"

#include "core/title_xml.h"

#include "core/media/utf8_path.h"

#include <glib.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>

// libxml2's BAD_CAST is a C cast (-Wold-style-cast).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"

namespace ustudio::titles::pack {

namespace fs = std::filesystem;

namespace {

struct DocFree
{
    void operator()(xmlDoc *doc) const
    {
        xmlFreeDoc(doc);
    }
};

std::optional<std::string> attr(const xmlNode *node, const char *name)
{
    xmlChar *value = xmlGetProp(node, BAD_CAST name);
    if (!value)
        return std::nullopt;
    std::string out(reinterpret_cast<const char *>(value));
    xmlFree(value);
    return out;
}

std::string text(const xmlNode *node)
{
    xmlChar *value = xmlNodeGetContent(node);
    std::string out = value ? reinterpret_cast<const char *>(value) : "";
    xmlFree(value);
    return out;
}

bool is(const xmlNode *node, const char *name)
{
    return node->type == XML_ELEMENT_NODE && xmlStrcmp(node->name, BAD_CAST name) == 0;
}

bool hasPrefix(std::string_view data, std::string_view prefix)
{
    return data.substr(0, prefix.size()) == prefix;
}

std::string extensionOf(const std::string &path)
{
    std::string ext = core::utf8String(core::pathFromUtf8(path).extension());
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

// What a pack may hold where, and what its bytes must look like.
std::optional<std::string> checkFile(const std::string &path, const std::string &data)
{
    const size_t slash = path.find('/');
    const std::string folder = slash == std::string::npos ? "" : path.substr(0, slash);
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (name.find('/') != std::string::npos)
        return path + " is in a subfolder; a pack's folders hold files only";
    const std::string ext = extensionOf(path);
    const auto png = [&] { return hasPrefix(data, "\x89PNG\r\n\x1a\n"); };
    if (folder.empty()) {
        if (path == "pack.xml")
            return std::nullopt;
        if (path == "LICENSE.txt") {
            if (data.find('\0') != std::string::npos ||
                !g_utf8_validate(data.data(), static_cast<gssize>(data.size()), nullptr))
                return std::string("LICENSE.txt isn't plain text");
            return std::nullopt;
        }
        return path + " isn't a file a pack may hold";
    }
    if (folder == "templates") {
        if (ext != ".ustitle")
            return path + " isn't a title (.ustitle)";
        if (!parseTitle(data))
            return path + " isn't a title the titles app can read";
        return std::nullopt;
    }
    if (folder == "previews") {
        if (ext != ".png" || !png())
            return path + " isn't a PNG picture";
        return std::nullopt;
    }
    if (folder == "images") {
        const bool jpeg = hasPrefix(data, "\xff\xd8\xff");
        if (!((ext == ".png" && png()) || ((ext == ".jpg" || ext == ".jpeg") && jpeg)))
            return path + " isn't a PNG or JPEG picture";
        return std::nullopt;
    }
    if (folder == "fonts") {
        const bool ttf = hasPrefix(data, std::string_view("\x00\x01\x00\x00", 4)) || hasPrefix(data, "true");
        const bool otf = hasPrefix(data, "OTTO");
        if (!((ext == ".ttf" && ttf) || (ext == ".otf" && (otf || ttf))))
            return path + " isn't a TrueType or OpenType font";
        return std::nullopt;
    }
    return path + " is in a folder a pack may not have";
}

bool validId(const std::string &id)
{
    if (id.empty() || id.size() > 128 || id.find("..") != std::string::npos || id.front() == '/' || id.back() == '/')
        return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '/';
    });
}

// "1.2.3" with optional "-pre" and "+build": its three numbers.
std::optional<std::array<long long, 3>> versionNumbers(std::string_view v)
{
    const size_t end = v.find_first_of("-+");
    const std::string_view core = v.substr(0, end);
    std::array<long long, 3> numbers{};
    size_t pos = 0;
    for (int i = 0; i < 3; ++i) {
        const size_t dot = core.find('.', pos);
        if ((i < 2) != (dot != std::string_view::npos))
            return std::nullopt;
        const std::string_view part =
            core.substr(pos, dot == std::string_view::npos ? std::string_view::npos : dot - pos);
        auto [ptr, ec] = std::from_chars(part.data(), part.data() + part.size(), numbers[static_cast<size_t>(i)]);
        if (part.empty() || ec != std::errc() || ptr != part.data() + part.size())
            return std::nullopt;
        pos = dot + 1;
    }
    return numbers;
}

// A template's picture reference, resolved from `base` (the template's
// folder in the pack) to a pack path: ".." climbs, but never above the
// pack's root; "" when it would, or when it's absolute or a URL.
std::string resolveInPack(std::string_view base, std::string_view relative)
{
    if (relative.empty() || relative.front() == '/' || relative.find("://") != std::string_view::npos)
        return {};
    std::vector<std::string> parts;
    const auto walk = [&](std::string_view path) {
        size_t pos = 0;
        while (pos <= path.size()) {
            const size_t slash = path.find('/', pos);
            const std::string_view part =
                path.substr(pos, slash == std::string_view::npos ? std::string_view::npos : slash - pos);
            if (part == "..") {
                if (parts.empty())
                    return false;
                parts.pop_back();
            } else if (!part.empty() && part != ".") {
                parts.emplace_back(part);
            }
            if (slash == std::string_view::npos)
                break;
            pos = slash + 1;
        }
        return true;
    };
    if (!walk(base) || !walk(relative))
        return {};
    std::string joined;
    for (const std::string &part : parts)
        joined += (joined.empty() ? "" : "/") + part;
    return normalisePath(joined);
}

// The folder a pack with `id` installs to: its id, made one safe name.
std::string folderName(const std::string &id)
{
    std::string name;
    for (char c : id)
        name += c == '/' ? std::string("--") : std::string(1, c);
    return name;
}

bool writeFile(const fs::path &path, const std::string &data)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

std::optional<std::string> readFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

std::string sha256(std::string_view data)
{
    gchar *hex =
        g_compute_checksum_for_data(G_CHECKSUM_SHA256, reinterpret_cast<const guchar *>(data.data()), data.size());
    std::string out = hex;
    g_free(hex);
    return out;
}

bool fontLicenceAllowed(std::string_view spdx)
{
    static const std::set<std::string_view> allowed = {"OFL-1.1", "Apache-2.0", "MIT",
                                                       "CC0-1.0", "UFL-1.0",    "CC-BY-4.0"};
    return allowed.contains(spdx);
}

std::string normalisePath(std::string_view path)
{
    if (path.empty() || path.front() == '/')
        return {};
    std::string out;
    size_t pos = 0;
    while (pos <= path.size()) {
        const size_t slash = path.find('/', pos);
        const std::string_view part =
            path.substr(pos, slash == std::string_view::npos ? std::string_view::npos : slash - pos);
        if (part == "..")
            return {};
        if (!part.empty() && part != ".") {
            for (unsigned char c : part)
                if (c < 0x20 || c == 0x7f || c == '\\' || c == ':')
                    return {};
            out += (out.empty() ? "" : "/") + std::string(part);
        }
        if (slash == std::string_view::npos)
            break;
        pos = slash + 1;
    }
    return out;
}

int compareVersions(std::string_view a, std::string_view b)
{
    const auto na = versionNumbers(a), nb = versionNumbers(b);
    if (!na || !nb)
        return a.compare(b) < 0 ? -1 : (a == b ? 0 : 1);
    if (*na != *nb)
        return *na < *nb ? -1 : 1;
    // Same numbers: a release is newer than its pre-release; build metadata
    // doesn't count.
    const auto pre = [](std::string_view v) {
        const std::string_view noBuild = v.substr(0, v.find('+'));
        const size_t dash = noBuild.find('-');
        return dash == std::string_view::npos ? std::string_view() : noBuild.substr(dash + 1);
    };
    const std::string_view pa = pre(a), pb = pre(b);
    if (pa == pb)
        return 0;
    if (pa.empty())
        return 1;
    if (pb.empty())
        return -1;
    return pa < pb ? -1 : 1;
}

std::expected<Manifest, std::string> parseManifest(std::string_view xml)
{
    std::unique_ptr<xmlDoc, DocFree> doc(xmlReadMemory(xml.data(), static_cast<int>(xml.size()), "pack.xml", nullptr,
                                                       XML_PARSE_NONET | XML_PARSE_NOBLANKS));
    if (!doc)
        return std::unexpected("pack.xml isn't well-formed XML");
    const xmlNode *root = xmlDocGetRootElement(doc.get());
    if (!root || !is(root, "pack"))
        return std::unexpected("pack.xml has no <pack>");
    Manifest m;
    const std::string format = attr(root, "format").value_or("");
    auto [ptr, ec] = std::from_chars(format.data(), format.data() + format.size(), m.format);
    if (format.empty() || ec != std::errc() || ptr != format.data() + format.size())
        return std::unexpected("pack.xml has no format");
    if (m.format != kFormat)
        return std::unexpected("the pack is in format " + format + ", which this version can't read");
    m.id = attr(root, "id").value_or("");
    m.version = attr(root, "version").value_or("");
    m.minAppVersion = attr(root, "min-app-version").value_or("");
    if (!validId(m.id))
        return std::unexpected("the pack's id isn't valid (lowercase letters, digits, - _ . /)");
    if (!versionNumbers(m.version))
        return std::unexpected("the pack's version isn't a version number (1.2.3)");
    for (const xmlNode *child = root->children; child; child = child->next) {
        if (child->type != XML_ELEMENT_NODE)
            continue;
        if (is(child, "title"))
            m.title = text(child);
        else if (is(child, "description"))
            m.description = text(child);
        else if (is(child, "author"))
            m.author = text(child);
        else if (is(child, "publisher"))
            m.publisher = text(child);
        else if (is(child, "licence"))
            m.licence = text(child);
        else if (is(child, "tag"))
            m.tags.push_back(text(child));
        else if (is(child, "file")) {
            ManifestFile file;
            file.path = attr(child, "path").value_or("");
            file.sha256 = attr(child, "sha256").value_or("");
            const std::string size = attr(child, "size").value_or("");
            auto [p, e] = std::from_chars(size.data(), size.data() + size.size(), file.size);
            if (size.empty() || e != std::errc() || p != size.data() + size.size() || file.sha256.size() != 64)
                return std::unexpected("pack.xml lists " + file.path + " without a size and SHA-256");
            m.files.push_back(std::move(file));
        } else if (is(child, "font")) {
            m.fonts.push_back({attr(child, "family").value_or(""), attr(child, "file").value_or(""),
                               attr(child, "licence").value_or("")});
        } else {
            return std::unexpected(std::string("pack.xml has an unknown <") +
                                   reinterpret_cast<const char *>(child->name) + ">");
        }
    }
    if (m.title.empty())
        return std::unexpected("the pack has no title");
    if (m.licence.empty())
        return std::unexpected("the pack has no licence");
    return m;
}

std::string writeManifest(const Manifest &m)
{
    std::unique_ptr<xmlDoc, DocFree> doc(xmlNewDoc(BAD_CAST "1.0"));
    xmlNode *root = xmlNewNode(nullptr, BAD_CAST "pack");
    xmlDocSetRootElement(doc.get(), root);
    xmlNewProp(root, BAD_CAST "format", BAD_CAST std::to_string(m.format).c_str());
    xmlNewProp(root, BAD_CAST "id", BAD_CAST m.id.c_str());
    xmlNewProp(root, BAD_CAST "version", BAD_CAST m.version.c_str());
    if (!m.minAppVersion.empty())
        xmlNewProp(root, BAD_CAST "min-app-version", BAD_CAST m.minAppVersion.c_str());
    const auto element = [&](const char *name, const std::string &value) {
        if (!value.empty())
            xmlNewTextChild(root, nullptr, BAD_CAST name, BAD_CAST value.c_str());
    };
    element("title", m.title);
    element("description", m.description);
    element("author", m.author);
    element("publisher", m.publisher);
    element("licence", m.licence);
    for (const std::string &tag : m.tags)
        element("tag", tag);
    for (const ManifestFile &file : m.files) {
        xmlNode *node = xmlNewChild(root, nullptr, BAD_CAST "file", nullptr);
        xmlNewProp(node, BAD_CAST "path", BAD_CAST file.path.c_str());
        xmlNewProp(node, BAD_CAST "size", BAD_CAST std::to_string(file.size).c_str());
        xmlNewProp(node, BAD_CAST "sha256", BAD_CAST file.sha256.c_str());
    }
    for (const ManifestFont &font : m.fonts) {
        xmlNode *node = xmlNewChild(root, nullptr, BAD_CAST "font", nullptr);
        xmlNewProp(node, BAD_CAST "family", BAD_CAST font.family.c_str());
        xmlNewProp(node, BAD_CAST "file", BAD_CAST font.file.c_str());
        xmlNewProp(node, BAD_CAST "licence", BAD_CAST font.licence.c_str());
    }
    xmlChar *buffer = nullptr;
    int size = 0;
    xmlDocDumpFormatMemoryEnc(doc.get(), &buffer, &size, "UTF-8", 1);
    std::string out(reinterpret_cast<const char *>(buffer), static_cast<size_t>(size));
    xmlFree(buffer);
    return out;
}

std::expected<ValidPack, std::string> validate(const std::vector<Entry> &entries, uint64_t archiveBytes,
                                               const Limits &limits)
{
    ValidPack pack;
    uint64_t total = 0;
    size_t files = 0;
    for (const Entry &entry : entries) {
        if (entry.kind == Entry::Kind::Link)
            return std::unexpected(entry.path + " is a link; a pack may hold files only");
        if (entry.kind == Entry::Kind::Other)
            return std::unexpected(entry.path + " isn't a file");
        const std::string path = normalisePath(entry.path);
        if (path.empty())
            return std::unexpected("“" + entry.path + "” isn't a path inside the pack");
        if (entry.kind == Entry::Kind::Directory) {
            if (path != "templates" && path != "previews" && path != "fonts" && path != "images")
                return std::unexpected(path + "/ is a folder a pack may not have");
            continue;
        }
        if (++files > limits.maxFiles)
            return std::unexpected("the pack has more than " + std::to_string(limits.maxFiles) + " files");
        if (entry.data.size() > limits.maxFileBytes)
            return std::unexpected(path + " is too big");
        total += entry.data.size();
        if (total > limits.maxTotalBytes)
            return std::unexpected("the pack unpacks to more than " + std::to_string(limits.maxTotalBytes >> 20) +
                                   " MB");
        if (pack.files.contains(path))
            return std::unexpected(path + " is in the pack twice");
        if (auto problem = checkFile(path, entry.data))
            return std::unexpected(*problem);
        pack.files.emplace(path, entry.data);
    }
    if (total > std::max<uint64_t>(archiveBytes, 1) * limits.maxRatio)
        return std::unexpected("the pack unpacks to far more than its size (a decompression bomb?)");
    auto manifestFile = pack.files.find("pack.xml");
    if (manifestFile == pack.files.end())
        return std::unexpected("the pack has no pack.xml");
    auto manifest = parseManifest(manifestFile->second);
    if (!manifest)
        return std::unexpected(manifest.error());
    pack.manifest = std::move(*manifest);

    // The manifest lists every other file, with its size and SHA-256.
    std::set<std::string> listed;
    for (const ManifestFile &file : pack.manifest.files) {
        const std::string path = normalisePath(file.path);
        if (path.empty() || path != file.path || path == "pack.xml")
            return std::unexpected("pack.xml lists “" + file.path + "”, which isn't a path inside the pack");
        auto found = pack.files.find(path);
        if (found == pack.files.end())
            return std::unexpected("pack.xml lists " + path + ", which isn't in the pack");
        if (found->second.size() != file.size || sha256(found->second) != file.sha256)
            return std::unexpected(path + " isn't the file pack.xml describes (its size or SHA-256 differs)");
        listed.insert(path);
    }
    for (const auto &[path, data] : pack.files)
        if (path != "pack.xml" && !listed.contains(path))
            return std::unexpected(path + " is in the pack but not in pack.xml");

    // Fonts: each one declared, with a licence that allows sharing it.
    std::set<std::string> declared;
    for (const ManifestFont &font : pack.manifest.fonts) {
        if (!pack.files.contains(font.file) || !font.file.starts_with("fonts/"))
            return std::unexpected("pack.xml declares the font " + font.file + ", which isn't in the pack");
        if (!fontLicenceAllowed(font.licence))
            return std::unexpected("the font " + font.file + " has the licence “" + font.licence +
                                   "”, which doesn't allow sharing it");
        declared.insert(font.file);
    }
    // Templates: a preview each, and pictures only from the pack.
    size_t templates = 0;
    for (const auto &[path, data] : pack.files) {
        if (path.starts_with("fonts/") && !declared.contains(path))
            return std::unexpected(path + " has no licence in pack.xml");
        if (!path.starts_with("templates/"))
            continue;
        ++templates;
        const std::string stem = core::utf8String(core::pathFromUtf8(path).stem());
        if (!pack.files.contains("previews/" + stem + ".png"))
            return std::unexpected(path + " has no preview (previews/" + stem + ".png)");
        auto doc = parseTitle(data);
        for (const Layer &layer : doc->document.layers) {
            if (layer.kind != LayerKind::Image || layer.src.empty())
                continue;
            const std::string picture = resolveInPack("templates", layer.src);
            if (picture.empty() || !picture.starts_with("images/") || !pack.files.contains(picture))
                return std::unexpected(path + " uses the picture “" + layer.src + "”, which isn't in the pack");
        }
    }
    if (templates == 0)
        return std::unexpected("the pack has no templates");
    return pack;
}

std::vector<InstalledPack> installedPacks(const std::string &library)
{
    std::vector<InstalledPack> out;
    std::error_code ec;
    const fs::path root = core::pathFromUtf8(library) / "packs";
    if (!fs::is_directory(root, ec))
        return out;
    for (const fs::directory_entry &entry : fs::directory_iterator(root, ec)) {
        const std::string name = core::utf8String(entry.path().filename());
        if (name.starts_with(".") || !entry.is_directory(ec))
            continue;
        auto xml = readFile(entry.path() / "pack.xml");
        if (!xml)
            continue;
        auto manifest = parseManifest(*xml);
        if (manifest)
            out.push_back({std::move(*manifest), core::utf8String(entry.path())});
    }
    std::sort(out.begin(), out.end(),
              [](const InstalledPack &a, const InstalledPack &b) { return a.manifest.title < b.manifest.title; });
    return out;
}

std::expected<InstalledPack, std::string> install(const ValidPack &pack, const std::string &library, Replace replace)
{
    const fs::path packs = core::pathFromUtf8(library) / "packs";
    const std::string name = folderName(pack.manifest.id);
    const fs::path folder = packs / core::pathFromUtf8(name);
    std::error_code ec;
    if (fs::exists(folder, ec)) {
        std::optional<Manifest> installed;
        if (auto xml = readFile(folder / "pack.xml"))
            if (auto m = parseManifest(*xml))
                installed = *m;
        const int order = installed ? compareVersions(pack.manifest.version, installed->version) : 1;
        if (replace == Replace::Never)
            return std::unexpected("“" + pack.manifest.title + "” is already installed");
        if (replace == Replace::IfNewer && order <= 0)
            return std::unexpected("“" + pack.manifest.title + "” " + (installed ? installed->version : "") +
                                   " is installed, and this one isn't newer");
    }
    fs::path part = packs / core::pathFromUtf8("." + name + ".part");
    fs::remove_all(part, ec);
    fs::create_directories(part / "fonts", ec);
    if (ec)
        return std::unexpected("can't write to " + core::utf8String(packs) + " (" + ec.message() + ")");
    const auto fail = [&](const std::string &why) -> std::expected<InstalledPack, std::string> {
        fs::remove_all(part, ec);
        return std::unexpected(why);
    };

    for (const auto &[path, data] : pack.files) {
        if (path == "pack.xml" || path == "LICENSE.txt" || path.starts_with("fonts/")) {
            if (!writeFile(part / core::pathFromUtf8(path), data))
                return fail("can't write " + path);
            continue;
        }
        if (!path.starts_with("templates/"))
            continue; // pictures and previews go with their templates
        // Each template a folder of its own, as My Templates are.
        const std::string stem = core::utf8String(core::pathFromUtf8(path).stem());
        const fs::path templateFolder = part / core::pathFromUtf8(stem);
        fs::create_directories(templateFolder / "images", ec);
        auto doc = parseTitle(data);
        TitleDocument title = doc->document;
        for (Layer &layer : title.layers) {
            if (layer.kind != LayerKind::Image || layer.src.empty())
                continue;
            const std::string picture = resolveInPack("templates", layer.src); // validated: images/<file>
            const std::string file = picture.substr(7);
            if (!writeFile(templateFolder / "images" / core::pathFromUtf8(file), pack.files.at(picture)))
                return fail("can't write " + picture);
            layer.src = "images/" + file;
        }
        if (const std::string error = saveTitle(title, core::utf8String(templateFolder / "template.ustitle"));
            !error.empty())
            return fail(error);
        if (!writeFile(templateFolder / "preview.png", pack.files.at("previews/" + stem + ".png")))
            return fail("can't write the preview of " + path);
    }

    // Swapped in: the old one aside, the new one in, the old one gone.
    fs::path old = packs / core::pathFromUtf8("." + name + ".old");
    fs::remove_all(old, ec);
    const bool hadOld = fs::exists(folder, ec);
    if (hadOld) {
        fs::rename(folder, old, ec);
        if (ec)
            return fail("can't replace the installed pack (" + ec.message() + ")");
    }
    fs::rename(part, folder, ec);
    if (ec) {
        if (hadOld)
            fs::rename(old, folder, ec);
        return fail("can't install the pack (" + ec.message() + ")");
    }
    fs::remove_all(old, ec);
    return InstalledPack{pack.manifest, core::utf8String(folder)};
}

std::expected<void, std::string> remove(const InstalledPack &pack)
{
    const fs::path folder = core::pathFromUtf8(pack.folder);
    std::error_code ec;
    if (!fs::is_regular_file(folder / "pack.xml", ec))
        return std::unexpected(pack.folder + " isn't an installed pack");
    fs::remove_all(folder, ec);
    if (ec)
        return std::unexpected("can't remove " + pack.folder + " (" + ec.message() + ")");
    return {};
}

std::expected<std::vector<Entry>, std::string> build(Manifest manifest, const std::vector<PackTemplate> &templates)
{
    if (templates.empty())
        return std::unexpected("a pack needs at least one template");
    std::map<std::string, std::string> files; // path -> bytes
    std::set<std::string> stems;
    for (const PackTemplate &entry : templates) {
        const fs::path folder = core::pathFromUtf8(entry.folder);
        auto read = readTitle(core::utf8String(folder / "template.ustitle"));
        if (!read)
            return std::unexpected(read.error());
        TitleDocument doc = read->document;
        std::string stem = core::utf8String(folder.filename());
        for (int n = 2; stems.contains(stem); ++n)
            stem = core::utf8String(folder.filename()) + "-" + std::to_string(n);
        stems.insert(stem);
        for (Layer &layer : doc.layers) {
            if (layer.kind != LayerKind::Image || layer.src.empty())
                continue;
            fs::path source = core::pathFromUtf8(layer.src);
            if (source.is_relative())
                source = folder / source;
            auto bytes = readFile(source);
            if (!bytes)
                return std::unexpected("the picture " + layer.src + " of “" + doc.name + "” isn't there");
            std::string name = core::utf8String(source.filename());
            // Two different pictures with one name: the second is renamed.
            for (int n = 2; files.contains("images/" + name) && files.at("images/" + name) != *bytes; ++n)
                name = core::utf8String(source.stem()) + "-" + std::to_string(n) + core::utf8String(source.extension());
            files["images/" + name] = *bytes;
            layer.src = "../images/" + name;
        }
        files["templates/" + stem + ".ustitle"] = writeTitle(doc);
        files["previews/" + stem + ".png"] = entry.previewPng;
    }
    manifest.format = kFormat;
    manifest.files.clear();
    for (const auto &[path, data] : files)
        manifest.files.push_back({path, data.size(), sha256(data)});
    std::vector<Entry> entries;
    entries.push_back({"pack.xml", Entry::Kind::File, writeManifest(manifest)});
    for (auto &[path, data] : files)
        entries.push_back({path, Entry::Kind::File, std::move(data)});
    return entries;
}

} // namespace ustudio::titles::pack

#pragma GCC diagnostic pop
