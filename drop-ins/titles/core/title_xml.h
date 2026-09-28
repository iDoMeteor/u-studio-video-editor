#pragma once

// The .ustitle file (doc 16): XML via libxml2, like project files (ADR-004),
// versioned from 1. Titles are untrusted input (a project names them, a
// user downloads templates): the reader refuses what it can't place and
// clamps numbers to sane ranges, and never fetches anything (no network, no
// external entities).

#include "title_document.h"

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace ustudio::titles {

constexpr int kTitleFormatVersion = 1;

// Whether `path` names a title file (".ustitle", any case).
bool isTitleFile(std::string_view path);

struct ReadResult
{
    TitleDocument document;
    // Things the file asks for that this version doesn't do (a newer
    // element, an unknown shape), each once. The title still renders.
    std::vector<std::string> warnings;
};

std::expected<ReadResult, std::string> parseTitle(std::string_view xml);
std::expected<ReadResult, std::string> readTitle(const std::string &path);

std::string writeTitle(const TitleDocument &doc);
// Atomically: a temporary file next to `path`, then a rename over it.
// Empty on success, else the reason.
std::string saveTitle(const TitleDocument &doc, const std::string &path);

} // namespace ustudio::titles
