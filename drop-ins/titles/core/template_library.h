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
#include <optional>
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

// A template's document for a title at `titlePath`: its layers, timing and
// fields, without the template's name, its pictures copied beside the
// title ("<title name> images/"). With no path (an untitled title), the
// pictures stay where they are, by absolute path.
std::expected<TitleDocument, std::string> templateDocument(const TemplateInfo &info, const std::string &titlePath);

// A new title at `destination` from a template: its layers, timing and
// fields, without the template's name, and its pictures copied beside the
// title ("<title name> images/"). Never overwrites a file.
std::expected<void, std::string> newTitleFromTemplate(const TemplateInfo &info, const std::string &destination);

// --- Update from template (T4.3) ----------------------------------------------

// Where a template is, as a title records it: "builtin:<id>", "user:<id>",
// or "pack:<pack folder>/<id>" for a template in <library>/packs/.
std::string templateRef(const TemplateInfo &info);

// A template's design, as a 64-bit FNV-1a hex digest of its file as
// writeTitle() writes it without its name, category or own reference:
// renaming a template doesn't change it.
std::string templateRevision(const TitleDocument &doc);

// The template `ref` names, if it's still there. `ref` is from a title file,
// so untrusted: each part must be a plain name (letters, digits, '-', '_',
// '.', not starting with '.'), else nothing.
std::optional<TemplateInfo> resolveTemplate(const std::string &ref, const std::string &builtInDir,
                                            const std::string &library);

// The template `title` was made from, when its design has changed since.
std::optional<TemplateInfo> changedTemplate(const TitleDocument &title, const std::string &builtInDir,
                                            const std::string &library);

struct TemplateUpdate
{
    TitleDocument document;
    std::vector<std::string> droppedFields; // the title's fields the template no longer has
};

// `title` brought up to `info`'s current design: the template's document
// (as templateDocument() makes it for `titlePath`) with the title's field
// text kept for every field of the same name.
std::expected<TemplateUpdate, std::string> updateFromTemplate(const TitleDocument &title, const TemplateInfo &info,
                                                              const std::string &titlePath);

// A folder name from `name`: lowercase letters, digits and dashes.
std::string templateSlug(const std::string &name);

} // namespace ustudio::titles
