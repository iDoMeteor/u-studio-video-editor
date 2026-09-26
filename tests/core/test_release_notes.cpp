// M4 G: Help's Release notes tab, from the app's AppStream metainfo.

#include "doctest.h"

#include "core/xml/release_notes.h"

#include <fstream>
#include <sstream>

using namespace ustudio::core;

TEST_CASE("release notes: every release, newest first, with its description")
{
    const char *xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<component type="desktop-application">
  <id>com.example.App</id>
  <releases>
    <release version="0.1.0" date="2026-01-10"/>
    <release version="0.3.0-beta.1" date="2026-03-01" type="development">
      <description>
        <p>A beta   with <em>inline</em>
           markup.</p>
        <ul>
          <li>Proxies</li>
          <li>Clip <code>transform</code></li>
        </ul>
        <ol><li>Numbered</li></ol>
      </description>
    </release>
    <release version="0.2.0" date="2026-02-01">
      <description><p>Second.</p></description>
    </release>
    <release version="0.0.9"/>
  </releases>
</component>)";
    const std::vector<ReleaseNote> notes = parseReleaseNotes(xml);
    REQUIRE(notes.size() == 4);
    CHECK(notes[0].version == "0.3.0-beta.1");
    CHECK(notes[1].version == "0.2.0");
    CHECK(notes[2].version == "0.1.0");
    CHECK(notes[3].version == "0.0.9"); // undated: last
    CHECK(notes[0].type == "development");
    CHECK(notes[1].type == "stable"); // AppStream's default
    CHECK(notes[0].date == "2026-03-01");
    REQUIRE(notes[0].blocks.size() == 4);
    CHECK(notes[0].blocks[0].kind == ReleaseNote::Block::Kind::Paragraph);
    CHECK(notes[0].blocks[0].text == "A beta with inline markup.");
    CHECK(notes[0].blocks[1].kind == ReleaseNote::Block::Kind::ListItem);
    CHECK(notes[0].blocks[1].text == "Proxies");
    CHECK(notes[0].blocks[2].text == "Clip transform");
    CHECK(notes[0].blocks[3].text == "Numbered");
    CHECK(notes[2].blocks.empty());
}

TEST_CASE("release notes: XML that doesn't parse gives none")
{
    CHECK(parseReleaseNotes("").empty());
    CHECK(parseReleaseNotes("<component><releases><release version=").empty());
    CHECK(parseReleaseNotes("<component/>").empty());
}

TEST_CASE("release notes: the app's own metainfo parses, the newest entry first")
{
    std::ifstream in(USTUDIO_METAINFO_XML);
    REQUIRE(in.good());
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::vector<ReleaseNote> notes = parseReleaseNotes(buffer.str());
    REQUIRE_FALSE(notes.empty());
    CHECK_FALSE(notes.front().version.empty());
    CHECK_FALSE(notes.front().blocks.empty()); // a releasable build has notes
}
