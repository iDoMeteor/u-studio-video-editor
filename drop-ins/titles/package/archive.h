#pragma once

// Template packs as files (doc 20, T4b): .zip, .tar.gz and .tgz through
// libarchive (ADR-020; the titles drop-in only, never src/). Reading
// stops at the limits as it goes, so a decompression bomb never unpacks
// in full; nothing is written until the pack has passed validate().
// A build without libarchive's headers says so instead.

#include "package/pack.h"

#include <expected>
#include <string>
#include <vector>

namespace ustudio::titles::pack {

// Whether this build reads and writes packs.
bool archivesSupported();

// An archive's entries, in memory, as validate() takes them.
std::expected<std::vector<Entry>, std::string> readArchive(const std::string &path, const Limits &limits = {});

// Writes `entries` as a .zip, or a .tar.gz for a .tar.gz or .tgz path,
// through a temporary file renamed over `path`.
std::expected<void, std::string> writeArchive(const std::vector<Entry> &entries, const std::string &path);

// Open Package: read, validate, install.
std::expected<InstalledPack, std::string> openPackage(const std::string &path, const std::string &library,
                                                      Replace replace);

// The pack in a file without installing it: for "what's inside" before
// installing, and the version it would replace.
std::expected<ValidPack, std::string> inspectPackage(const std::string &path);

} // namespace ustudio::titles::pack
