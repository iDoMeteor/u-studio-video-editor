#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "app/action_registry.h"
#include "app/ui_hints.h"

#include <cstring>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

using namespace ustudio::app;

TEST_CASE("ui hints: ids are unique and every hint has a category and title")
{
    std::set<std::string> ids;
    for (const HintSpec &hint : hintSpecs()) {
        INFO(hint.id);
        CHECK(ids.insert(hint.id).second);
        CHECK(std::strlen(hint.category) > 0);
        CHECK(std::strlen(hint.title) > 0);
    }
}

TEST_CASE("ui hints: every named action exists, and no text types a shortcut in by hand")
{
    for (const HintSpec &hint : hintSpecs()) {
        INFO(hint.id);
        if (hint.action != nullptr) {
            bool found = false;
            for (const ActionSpec &spec : actionSpecs())
                found = found || std::strcmp(spec.name, hint.action) == 0;
            CHECK(found);
        }
        // A shortcut written into the text goes stale when the binding
        // changes; name the action instead.
        for (const char *text : {hint.title, hint.detail, hint.gesture}) {
            if (text == nullptr)
                continue;
            std::string s = text;
            CHECK(s.find("Ctrl+") == std::string::npos);
            CHECK(s.find("Alt+") == std::string::npos);
            CHECK(s.find("Shift+") == std::string::npos);
        }
    }
}

TEST_CASE("ui hints: tooltip text appends the action's shortcut and the detail")
{
    CHECK(tooltipText("header.undo") == "Undo (Ctrl+Z)");
    CHECK(tooltipText("transport.split") == "Split the active track's clip at the playhead (X)");
    CHECK(tooltipText("track-menu.delete-clip") == "Delete clip\nLeaves a gap; nothing else moves");
    CHECK(tooltipText("no.such-hint") == "no.such-hint");
}

TEST_CASE("ui hints: a drop-in's hints are added after the shell's; duplicates can't replace them")
{
    size_t before = hintSpecs().size();
    registerHints({
        {"test-dropin.glow", "Test drop-in", "Add glow", "Softly", "undo", nullptr},
        {"header.undo", "Test drop-in", "Hijacked", nullptr, nullptr, nullptr},
    });
    REQUIRE(hintSpecs().size() == before + 1);
    CHECK(std::string(hintSpecs().back().id) == "test-dropin.glow");
    CHECK(tooltipText("test-dropin.glow") == "Add glow (Ctrl+Z)\nSoftly");
    CHECK(tooltipText("header.undo") == "Undo (Ctrl+Z)");
}

TEST_CASE("ui hints: every id the shell passes to setTooltip() is registered")
{
    std::ifstream in(USTUDIO_APP_WINDOW_CPP);
    REQUIRE(in.good());
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string source = buffer.str();

    // setTooltip(widget, "id"), and the transport buttons' hint-id argument.
    std::regex idPattern(R"re("((?:header|transport|timeline|track-menu|media)\.[a-z0-9-]+)")re");
    int seen = 0;
    for (auto it = std::sregex_iterator(source.begin(), source.end(), idPattern); it != std::sregex_iterator(); ++it) {
        std::string id = (*it)[1];
        INFO(id);
        CHECK(findHint(id) != nullptr);
        ++seen;
    }
    CHECK(seen > 30);
}
