#include "core/xml/release_notes.h"

#include <libxml/parser.h>
#include <libxml/tree.h>

#include <algorithm>
#include <cctype>

namespace ustudio::core {

namespace {

bool named(const xmlNode *node, const char *name)
{
    return node->type == XML_ELEMENT_NODE && xmlStrcmp(node->name, reinterpret_cast<const xmlChar *>(name)) == 0;
}

std::string attribute(xmlNode *node, const char *name)
{
    xmlChar *value = xmlGetProp(node, reinterpret_cast<const xmlChar *>(name));
    std::string out = value ? reinterpret_cast<const char *>(value) : "";
    xmlFree(value);
    return out;
}

// The element's text with any inline markup (<em>, <code>) flattened and
// whitespace runs collapsed, as AppStream clients show it.
std::string text(xmlNode *node)
{
    xmlChar *content = xmlNodeGetContent(node);
    std::string raw = content ? reinterpret_cast<const char *>(content) : "";
    xmlFree(content);
    std::string out;
    bool space = false;
    for (char c : raw) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            space = !out.empty();
            continue;
        }
        if (space)
            out += ' ';
        space = false;
        out += c;
    }
    return out;
}

void readDescription(xmlNode *description, ReleaseNote &note)
{
    for (xmlNode *child = description->children; child; child = child->next) {
        if (named(child, "p")) {
            note.blocks.push_back({ReleaseNote::Block::Kind::Paragraph, text(child)});
        } else if (named(child, "ul") || named(child, "ol")) {
            for (xmlNode *item = child->children; item; item = item->next)
                if (named(item, "li"))
                    note.blocks.push_back({ReleaseNote::Block::Kind::ListItem, text(item)});
        }
    }
}

} // namespace

std::vector<ReleaseNote> parseReleaseNotes(std::string_view metainfoXml)
{
    std::vector<ReleaseNote> notes;
    // Our own resource, but parsed as untrusted anyway: no network, no
    // entity expansion (libxml2's defaults), no DTD loading.
    xmlDocPtr doc = xmlReadMemory(metainfoXml.data(), static_cast<int>(metainfoXml.size()), "metainfo.xml", nullptr,
                                  XML_PARSE_NONET | XML_PARSE_NOBLANKS | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc)
        return notes;
    xmlNode *root = xmlDocGetRootElement(doc);
    for (xmlNode *section = root ? root->children : nullptr; section; section = section->next) {
        if (!named(section, "releases"))
            continue;
        for (xmlNode *release = section->children; release; release = release->next) {
            if (!named(release, "release"))
                continue;
            ReleaseNote note;
            note.version = attribute(release, "version");
            note.date = attribute(release, "date");
            note.type = attribute(release, "type");
            if (note.type.empty())
                note.type = "stable";
            for (xmlNode *child = release->children; child; child = child->next)
                if (named(child, "description"))
                    readDescription(child, note);
            notes.push_back(std::move(note));
        }
    }
    xmlFreeDoc(doc);
    // ISO dates sort as strings; undated entries go last.
    std::stable_sort(notes.begin(), notes.end(), [](const ReleaseNote &a, const ReleaseNote &b) {
        if (a.date.empty() != b.date.empty())
            return b.date.empty();
        return a.date > b.date;
    });
    return notes;
}

} // namespace ustudio::core
