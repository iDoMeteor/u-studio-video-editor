#pragma once

// M4 G: the Help dialog's Release notes tab, read from the app's own
// AppStream metainfo (data/com.ustudio.VideoEditor.metainfo.xml, compiled
// into the GResource). One source for the notes the software centre shows
// and the ones Help shows. Only releasable builds get an entry (README,
// "Release notes").

#include <string>
#include <string_view>
#include <vector>

namespace ustudio::core {

struct ReleaseNote
{
    struct Block
    {
        enum class Kind
        {
            Paragraph,
            ListItem,
        } kind = Kind::Paragraph;
        std::string text; // whitespace collapsed, inline markup flattened
    };

    std::string version; // "0.49.0-beta.1"
    std::string date;    // ISO 8601 as written ("2026-09-25"), may be empty
    std::string type;    // AppStream's: "stable" (the default) or "development"
    std::vector<Block> blocks;
};

// Every <release> under <releases>, newest first: by date, then in file
// order. Empty for XML that doesn't parse (the tab then says so).
std::vector<ReleaseNote> parseReleaseNotes(std::string_view metainfoXml);

} // namespace ustudio::core
