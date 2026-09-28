#pragma once

// Title templates (doc 16, T4): the read-only built-ins shipped with the
// titles drop-in, and the user's own library, "My Templates". Each user
// template is a folder, <library>/<id>/, holding template.ustitle, the
// pictures it uses (images/) and, once the app has drawn it, preview.png:
// the same shape as a template in a pack (doc 20), so T4b's packs install
// into it. Built-ins are flat: <dir>/<id>.ustitle and <id>.png.
//
// Std and libxml2 only; the caller says where the directories are
// (src/platform's data directory, or the drop-in's installed data).

#include "title_document.h"

#include <expected>
#include <string>
#include <vector>

namespace ustudio::titles {

struct TemplateInfo
{
    std::string id;             // the folder's (or built-in file's) name
    std::string name, category; // from the file; the id when it has no name
    std::string path;           // the .ustitle
    std::string preview;        // preview.png, or "" when there's none yet
    std::string folder;         // a user template's own folder; "" for a built-in
    bool builtIn = false;

    bool operator==(const TemplateInfo &) const = default;
};

// Every template in `dir`, sorted by category then name. A file that doesn't
// read is left out. A missing directory is an empty list.
std::vector<TemplateInfo> listTemplates(const std::string &dir, bool builtIn);

// `doc` saved into `library` as a new template called `name` (also written
// into the file), with its pictures copied in: an id made from the name,
// never an existing folder. Written to a temporary folder and renamed, so
// a failure leaves nothing.
std::expected<TemplateInfo, std::string> saveTemplate(const TitleDocument &doc, const std::string &library,
                                                      const std::string &name);

// A user template's new name (its folder stays).
std::expected<TemplateInfo, std::string> renameTemplate(const TemplateInfo &info, const std::string &name);

// A copy of any template (a built-in too: how one is edited) in `library`.
std::expected<TemplateInfo, std::string> duplicateTemplate(const TemplateInfo &info, const std::string &library,
                                                           const std::string &name);

// Deletes a user template's folder. A built-in can't be removed.
std::expected<void, std::string> removeTemplate(const TemplateInfo &info);

// A new title at `destination` from a template: its layers, timing and
// fields, without the template's name, and its pictures copied beside the
// title ("<title name> images/"). Never overwrites a file.
std::expected<void, std::string> newTitleFromTemplate(const TemplateInfo &info, const std::string &destination);

// A folder name from `name`: lowercase letters, digits and dashes.
std::string templateSlug(const std::string &name);

} // namespace ustudio::titles
