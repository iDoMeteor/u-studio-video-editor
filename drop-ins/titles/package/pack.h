#pragma once

// Template packs (doc 20, ADR-020; T4b): a .zip or .tar.gz of templates
// with a pack.xml manifest. This is everything but the archive format
// itself (archive.h): the manifest, the validator that every pack passes
// before anything is written, and installing into the user's template
// library.
//
// A pack is untrusted input from strangers. The validator works on the
// entries an archive reader produced, in memory, and rejects the whole
// pack with a reason a person can read if anything is off. Nothing in a
// pack is ever executed: .ustitle has no scripting.

#include "core/title_document.h"

#include <cstdint>
#include <expected>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace ustudio::titles::pack {

constexpr int kFormat = 1;

struct ManifestFile
{
    std::string path; // "templates/lower-third.ustitle"
    uint64_t size = 0;
    std::string sha256; // lowercase hex
    bool operator==(const ManifestFile &) const = default;
};

struct ManifestFont
{
    std::string family, file, licence; // file: "fonts/Anton-Regular.ttf"; licence: an SPDX id
    bool operator==(const ManifestFont &) const = default;
};

struct Manifest
{
    int format = kFormat;
    std::string id;      // "unicorn-tears/streamer-basics": stable across versions
    std::string version; // SemVer
    std::string title, description, author, publisher, licence, minAppVersion;
    std::vector<std::string> tags;
    std::vector<ManifestFile> files; // every file but pack.xml
    std::vector<ManifestFont> fonts;
    bool operator==(const Manifest &) const = default;
};

std::expected<Manifest, std::string> parseManifest(std::string_view xml);
std::string writeManifest(const Manifest &manifest);

// One entry as an archive reader saw it.
struct Entry
{
    enum class Kind
    {
        File,
        Directory,
        Link, // symbolic or hard
        Other // device, fifo, ...
    };
    std::string path; // as stored in the archive
    Kind kind = Kind::File;
    std::string data; // a file's bytes
};

struct Limits
{
    size_t maxFiles = 512;
    uint64_t maxFileBytes = 32ull << 20;   // one file
    uint64_t maxTotalBytes = 128ull << 20; // all files, unpacked
    uint64_t maxRatio = 100;               // unpacked bytes per archive byte
};

// A pack that passed: its manifest and its files by normalised path.
struct ValidPack
{
    Manifest manifest;
    std::map<std::string, std::string> files; // path -> bytes, pack.xml included
};

// The whole of doc 20's "Validation": every check, before anything is
// written. `archiveBytes` is the archive's size on disk (for the ratio).
std::expected<ValidPack, std::string> validate(const std::vector<Entry> &entries, uint64_t archiveBytes,
                                               const Limits &limits = {});

// A normalised relative path ("templates/a.ustitle"), or "" when the path
// is absolute, climbs out (".."), is empty, or has a backslash or a
// control character. "./" prefixes and repeated slashes are dropped.
std::string normalisePath(std::string_view path);

// SemVer order of two versions: <0, 0, >0. Pre-release and build parts are
// compared after the numbers (a pre-release is older than its release).
int compareVersions(std::string_view a, std::string_view b);

// SHA-256 of `data`, lowercase hex.
std::string sha256(std::string_view data);

// The fonts licences a pack may carry (redistribution allowed).
bool fontLicenceAllowed(std::string_view spdx);

// --- The user's library ------------------------------------------------------

// The user's template library, My Templates: $XDG_DATA_HOME/ustudio/titles/
// templates. The designer, the editor and u-studio-share all use it.
std::string templatesLibrary();

// Packs install into <library>/packs/<folder>/, each template a folder of
// its own (<folder>/<template>/template.ustitle, images/, preview.png) as
// My Templates are, with the pack's fonts in <folder>/fonts/ and its
// pack.xml kept beside them.
struct InstalledPack
{
    Manifest manifest;
    std::string folder; // absolute
};
std::vector<InstalledPack> installedPacks(const std::string &library);

enum class Replace
{
    Never,   // an installed pack with this id is an error
    IfNewer, // replace an older version; an equal or newer one is an error
    Always,  // the user confirmed (a downgrade, or the same version again)
};

// Installs `pack`: written to a temporary folder beside its place, then
// renamed over it, so a failure leaves the library as it was.
std::expected<InstalledPack, std::string> install(const ValidPack &pack, const std::string &library, Replace replace);

// Removes an installed pack's folder (only one that holds a pack.xml).
std::expected<void, std::string> remove(const InstalledPack &pack);

// A pack from templates in the library (My Templates or other packs):
// every file the archive will hold, pack.xml first, with sizes and
// SHA-256s filled in. `previews` holds each template's PNG by its folder
// path (the caller renders them). The manifest's files and fonts are
// computed; the rest of `manifest` is used as given.
struct PackTemplate
{
    std::string folder;     // the template's folder (holds template.ustitle)
    std::string previewPng; // its preview, PNG bytes
};
std::expected<std::vector<Entry>, std::string> build(Manifest manifest, const std::vector<PackTemplate> &templates);

} // namespace ustudio::titles::pack
