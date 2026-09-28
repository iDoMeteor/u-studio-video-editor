#include "captions.h"

#include "clip_fields.h"

#include "core/commands/primitives.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <optional>

namespace ustudio::titles::captions {

namespace {

void appendUtf8(std::string &out, uint32_t cp)
{
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool validUtf8(std::string_view s)
{
    size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        size_t extra = 0;
        uint32_t min = 0;
        if (c < 0x80) {
            ++i;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            extra = 1;
            min = 0x80;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            min = 0x800;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            min = 0x10000;
        } else {
            return false;
        }
        if (i + extra >= s.size())
            return false; // cut off
        uint32_t cp = c & (0x3F >> extra);
        for (size_t k = 1; k <= extra; ++k) {
            const auto next = static_cast<unsigned char>(s[i + k]);
            if ((next & 0xC0) != 0x80)
                return false;
            cp = (cp << 6) | (next & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000))
            return false;
        i += extra + 1;
    }
    return true;
}

// Windows-1252's 0x80..0x9F (0: undefined, kept as U+FFFD).
constexpr std::array<uint16_t, 32> kCp1252 = {0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                              0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
                                              0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                              0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178};

std::string trim(std::string_view s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        --b;
    return std::string(s.substr(a, b - a));
}

// "01:02:03,456", "01:02:03.456" or "02:03.456": milliseconds.
std::optional<int64_t> timestamp(std::string_view t)
{
    std::array<int64_t, 3> parts{};
    int count = 0;
    size_t i = 0;
    int64_t value = 0;
    int digits = 0;
    for (; i < t.size(); ++i) {
        const char c = t[i];
        if (c >= '0' && c <= '9') {
            value = value * 10 + (c - '0');
            if (++digits > 6)
                return std::nullopt;
        } else if (c == ':') {
            if (digits == 0 || count >= 2)
                return std::nullopt;
            parts[static_cast<size_t>(count++)] = value;
            value = 0;
            digits = 0;
        } else if (c == ',' || c == '.') {
            break;
        } else {
            return std::nullopt;
        }
    }
    if (digits == 0 || count < 1 || i >= t.size())
        return std::nullopt;
    const int64_t seconds = value;
    std::string_view fraction = t.substr(i + 1);
    if (fraction.size() != 3 ||
        !std::all_of(fraction.begin(), fraction.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return std::nullopt;
    const int64_t ms = (fraction[0] - '0') * 100 + (fraction[1] - '0') * 10 + (fraction[2] - '0');
    const int64_t hours = count == 2 ? parts[0] : 0, minutes = count == 2 ? parts[1] : parts[0];
    if (minutes > 59 || seconds > 59)
        return std::nullopt;
    const int64_t total = ((hours * 60 + minutes) * 60 + seconds) * 1000 + ms;
    return total > kMaxMs ? std::nullopt : std::optional<int64_t>(total);
}

// "START --> END [settings]"
std::optional<std::pair<int64_t, int64_t>> timing(const std::string &line)
{
    const size_t arrow = line.find("-->");
    if (arrow == std::string::npos)
        return std::nullopt;
    const std::string startText = trim(line.substr(0, arrow));
    std::string rest = trim(line.substr(arrow + 3));
    const size_t space = rest.find_first_of(" \t");
    const std::string endText = space == std::string::npos ? rest : rest.substr(0, space);
    auto start = timestamp(startText), end = timestamp(endText);
    if (!start || !end)
        return std::nullopt;
    return std::pair{*start, *end};
}

void decodeEntities(std::string &text)
{
    static const std::map<std::string, uint32_t> named = {{"amp", '&'},    {"lt", '<'},    {"gt", '>'},
                                                          {"quot", '"'},   {"apos", '\''}, {"nbsp", 0xA0},
                                                          {"lrm", 0x200E}, {"rlm", 0x200F}};
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '&') {
            const size_t semi = text.find(';', i);
            if (semi != std::string::npos && semi - i <= 10) {
                const std::string name = text.substr(i + 1, semi - i - 1);
                uint32_t cp = 0;
                bool ok = false;
                if (auto it = named.find(name); it != named.end()) {
                    cp = it->second;
                    ok = true;
                } else if (name.size() > 1 && name[0] == '#') {
                    const bool hex = name[1] == 'x' || name[1] == 'X';
                    const std::string digits = name.substr(hex ? 2 : 1);
                    if (!digits.empty() && digits.size() <= 7 &&
                        std::all_of(digits.begin(), digits.end(), [hex](char c) {
                            return hex ? std::isxdigit(static_cast<unsigned char>(c))
                                       : std::isdigit(static_cast<unsigned char>(c));
                        })) {
                        cp = static_cast<uint32_t>(std::stoul(digits, nullptr, hex ? 16 : 10));
                        ok = cp > 0 && cp < 0x110000 && !(cp >= 0xD800 && cp < 0xE000);
                    }
                }
                if (ok) {
                    appendUtf8(out, cp);
                    i = semi;
                    continue;
                }
            }
        }
        out += text[i];
    }
    text = std::move(out);
}

// Tags: <b>, <i>, <u> kept (normalised); <v Speaker> read; everything else,
// and SRT's {\...} overrides, dropped with its text kept.
std::string cleanText(const std::string &raw, std::string &speaker)
{
    std::string out;
    std::map<char, int> open;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '{' && i + 1 < raw.size() && raw[i + 1] == '\\') {
            const size_t close = raw.find('}', i);
            if (close != std::string::npos) {
                i = close;
                continue;
            }
        }
        if (raw[i] != '<') {
            out += raw[i];
            continue;
        }
        const size_t close = raw.find('>', i);
        if (close == std::string::npos) {
            out += raw[i];
            continue;
        }
        std::string tag = raw.substr(i + 1, close - i - 1);
        const bool closing = !tag.empty() && tag[0] == '/';
        if (closing)
            tag.erase(0, 1);
        std::string name;
        for (char c : tag) {
            if (std::isalpha(static_cast<unsigned char>(c)))
                name += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            else
                break;
        }
        const bool timestampTag = !tag.empty() && std::isdigit(static_cast<unsigned char>(tag[0]));
        if (name.empty() && !timestampTag) {
            out += raw[i]; // "a < b": not a tag
            continue;
        }
        if ((name == "b" || name == "i" || name == "u") && name.size() == 1) {
            const char kind = name[0];
            if (closing) {
                if (open[kind] > 0) {
                    --open[kind];
                    out += std::string("</") + kind + ">";
                }
            } else {
                ++open[kind];
                out += std::string("<") + kind + ">";
            }
        } else if (name == "v" && !closing && speaker.empty()) {
            const size_t space = tag.find(' ');
            if (space != std::string::npos)
                speaker = trim(tag.substr(space + 1));
        }
        i = close;
    }
    for (const auto &[kind, count] : open)
        for (int k = 0; k < count; ++k)
            out += std::string("</") + kind + ">";
    decodeEntities(out);
    decodeEntities(speaker);
    // Blank lines inside a cue (after tags went) collapse; ends trimmed.
    std::string lines, line;
    for (size_t i = 0; i <= out.size(); ++i) {
        if (i == out.size() || out[i] == '\n') {
            std::string t = trim(line);
            if (!t.empty())
                lines += (lines.empty() ? "" : "\n") + t;
            line.clear();
        } else {
            line += out[i];
        }
    }
    return lines;
}

std::vector<std::string> splitLines(const std::string &text)
{
    std::vector<std::string> lines;
    std::string line;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\r') {
            lines.push_back(line);
            line.clear();
            if (i + 1 < text.size() && text[i + 1] == '\n')
                ++i;
        } else if (c == '\n') {
            lines.push_back(line);
            line.clear();
        } else {
            line += c;
        }
    }
    lines.push_back(line);
    return lines;
}

} // namespace

std::string toUtf8(std::string_view bytes, std::string &warning, std::string &error)
{
    warning.clear();
    error.clear();
    if (bytes.size() >= 2 &&
        ((bytes[0] == '\xFF' && bytes[1] == '\xFE') || (bytes[0] == '\xFE' && bytes[1] == '\xFF'))) {
        const bool little = bytes[0] == '\xFF';
        std::string out;
        for (size_t i = 2; i + 1 < bytes.size(); i += 2) {
            const auto a = static_cast<unsigned char>(bytes[i]), b = static_cast<unsigned char>(bytes[i + 1]);
            uint32_t unit = little ? (a | (b << 8)) : ((a << 8) | b);
            if (unit >= 0xD800 && unit < 0xDC00 && i + 3 < bytes.size()) {
                const auto c = static_cast<unsigned char>(bytes[i + 2]), d = static_cast<unsigned char>(bytes[i + 3]);
                const uint32_t low = little ? (c | (d << 8)) : ((c << 8) | d);
                if (low >= 0xDC00 && low < 0xE000) {
                    unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                    i += 2;
                }
            }
            if (unit == 0) {
                error = "the file isn't text";
                return {};
            }
            appendUtf8(out, unit);
        }
        return out;
    }
    if (bytes.find('\0') != std::string_view::npos) {
        error = "the file isn't text";
        return {};
    }
    if (bytes.substr(0, 3) == "\xEF\xBB\xBF")
        bytes.remove_prefix(3);
    if (validUtf8(bytes))
        return std::string(bytes);
    warning = "read as Windows-1252 (it isn't UTF-8)";
    std::string out;
    for (char ch : bytes) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x80)
            out += ch;
        else if (c < 0xA0)
            appendUtf8(out, kCp1252[c - 0x80] ? kCp1252[c - 0x80] : 0xFFFD);
        else
            appendUtf8(out, c);
    }
    return out;
}

std::expected<Parsed, std::string> parse(std::string_view bytes)
{
    if (bytes.size() > kMaxBytes)
        return std::unexpected("the file is over " + std::to_string(kMaxBytes >> 20) + " MB");
    std::string warning, error;
    const std::string text = toUtf8(bytes, warning, error);
    if (!error.empty())
        return std::unexpected(error);
    Parsed parsed;
    if (!warning.empty())
        parsed.warnings.push_back(warning);
    const std::vector<std::string> lines = splitLines(text);
    const bool vtt = !lines.empty() && lines[0].starts_with("WEBVTT");
    const auto skip = [&](int line, const std::string &why) {
        if (++parsed.skipped == 1) {
            parsed.firstSkippedLine = line;
            parsed.firstSkippedWhy = why;
        }
    };
    size_t i = vtt ? 1 : 0;
    size_t timed = 0; // blocks with a timing line: none means not a subtitle file
    while (i < lines.size()) {
        // A block: lines up to a blank one.
        while (i < lines.size() && trim(lines[i]).empty())
            ++i;
        if (i >= lines.size())
            break;
        const size_t begin = i;
        while (i < lines.size() && !trim(lines[i]).empty())
            ++i;
        std::vector<std::string> block(lines.begin() + static_cast<long>(begin), lines.begin() + static_cast<long>(i));
        const int lineNo = static_cast<int>(begin) + 1;
        if (vtt && (block[0].starts_with("NOTE") || block[0].starts_with("STYLE") || block[0].starts_with("REGION")))
            continue;
        // The timing line: the first with "-->" (an SRT index or a VTT
        // identifier may come before it, and counts for nothing).
        size_t t = 0;
        while (t < block.size() && block[t].find("-->") == std::string::npos)
            ++t;
        if (t == block.size() || t > 1) {
            skip(lineNo, "no timing line");
            continue;
        }
        ++timed;
        auto times = timing(block[t]);
        if (!times) {
            skip(lineNo + static_cast<int>(t), "a timestamp isn't one");
            continue;
        }
        if (times->second <= times->first) {
            skip(lineNo + static_cast<int>(t), "it ends before it starts");
            continue;
        }
        std::string raw;
        for (size_t k = t + 1; k < block.size(); ++k)
            raw += (k == t + 1 ? "" : "\n") + block[k];
        Cue cue;
        cue.startMs = times->first;
        cue.endMs = times->second;
        cue.line = lineNo;
        cue.text = cleanText(raw, cue.speaker);
        std::string words = cue.text;
        for (const char *tag : {"<b>", "</b>", "<i>", "</i>", "<u>", "</u>"})
            for (size_t at; (at = words.find(tag)) != std::string::npos;)
                words.erase(at, std::string_view(tag).size());
        if (trim(words).empty()) {
            skip(lineNo, "it has no words");
            continue;
        }
        parsed.cues.push_back(std::move(cue));
        if (parsed.cues.size() > kMaxCues)
            return std::unexpected("the file has over " + std::to_string(kMaxCues) + " cues");
    }
    if (parsed.cues.empty())
        return std::unexpected(timed ? "none of its cues can be read (line " + std::to_string(parsed.firstSkippedLine) +
                                           ": " + parsed.firstSkippedWhy + ")"
                                     : "it has no cues");
    return parsed;
}

core::FrameIndex frameAt(int64_t ms, core::Rational fps)
{
    // Nearest frame at the exact rate: ms * num / (1000 * den), rounded. In
    // 64 bits: ms is at most a day (8.64e7) and a rate's numerator is well
    // under 1e9, so twice their product stays under 2^63.
    if (fps.num <= 0 || fps.den <= 0)
        return 0;
    const int64_t twice = ms * static_cast<int64_t>(fps.num) * 2 + 1000 * static_cast<int64_t>(fps.den);
    return static_cast<core::FrameIndex>(twice / (2000 * static_cast<int64_t>(fps.den)));
}

std::vector<Placed> place(const std::vector<Cue> &cues, core::Rational fps)
{
    std::vector<const Cue *> order;
    for (const Cue &cue : cues)
        order.push_back(&cue);
    std::stable_sort(order.begin(), order.end(), [](const Cue *a, const Cue *b) { return a->startMs < b->startMs; });
    std::vector<Placed> out;
    std::vector<core::FrameIndex> laneEnd; // the frame after each lane's last cue
    for (const Cue *cue : order) {
        Placed p;
        p.cue = cue;
        p.position = frameAt(cue->startMs, fps);
        const core::FrameIndex end = std::max(frameAt(cue->endMs, fps), p.position + 1);
        p.length = end - p.position;
        size_t lane = 0;
        while (lane < laneEnd.size() && laneEnd[lane] > p.position)
            ++lane;
        if (lane == laneEnd.size())
            laneEnd.push_back(0);
        laneEnd[lane] = end;
        p.lane = lane;
        out.push_back(p);
    }
    return out;
}

ImportCaptions::ImportCaptions(core::Asset titleAsset, std::vector<Placed> placed, std::string name)
    : m_titleAsset(std::move(titleAsset)), m_name(std::move(name))
{
    // Own the cues, so the command outlives the parse.
    m_cues.reserve(placed.size());
    for (const Placed &p : placed)
        m_cues.push_back(*p.cue);
    for (size_t i = 0; i < placed.size(); ++i) {
        placed[i].cue = &m_cues[i];
        m_placed.push_back(placed[i]);
    }
}

bool ImportCaptions::apply(core::Model &model)
{
    // Redo: the same steps again, so every id comes back the same.
    if (!m_done.empty()) {
        for (size_t i = 0; i < m_done.size(); ++i) {
            if (!m_done[i]->apply(model)) {
                for (size_t k = i; k-- > 0;)
                    m_done[k]->revert(model);
                return false;
            }
        }
        return true;
    }
    const auto run = [&](std::unique_ptr<core::Command> step) -> core::Command * {
        if (!step->apply(model))
            return nullptr;
        m_done.push_back(std::move(step));
        return m_done.back().get();
    };
    const auto fail = [&] {
        for (auto it = m_done.rbegin(); it != m_done.rend(); ++it)
            (*it)->revert(model);
        m_done.clear();
        m_tracks.clear();
        m_clips.clear();
        return false;
    };
    auto *asset = static_cast<core::AddAsset *>(run(std::make_unique<core::AddAsset>(m_titleAsset)));
    if (!asset)
        return fail();
    m_asset = asset->assetId();
    size_t lanes = 0;
    for (const Placed &p : m_placed)
        lanes = std::max(lanes, p.lane + 1);
    // Above everything: the last lane in first, so "Captions" ends on top.
    m_tracks.assign(lanes, core::TrackId{});
    for (size_t lane = lanes; lane-- > 0;) {
        const std::string name = lane == 0 ? "Captions" : "Captions " + std::to_string(lane + 1);
        auto *track =
            static_cast<core::AddTrack *>(run(std::make_unique<core::AddTrack>(core::Track::Kind::Video, 0, name)));
        if (!track)
            return fail();
        m_tracks[lane] = track->trackId();
    }
    for (const Placed &p : m_placed) {
        auto *insert = static_cast<core::InsertClip *>(
            run(std::make_unique<core::InsertClip>(m_tracks[p.lane], m_asset, p.position, 0, p.length - 1)));
        if (!insert)
            return fail();
        m_clips.push_back(insert->clipId());
        std::map<std::string, std::string> fields = {{"caption", p.cue->text}};
        if (!p.cue->speaker.empty())
            fields["speaker"] = p.cue->speaker;
        if (!run(std::make_unique<SetClipFields>(insert->clipId(), std::move(fields))))
            return fail();
        // Named by its words, so the timeline reads like the script.
        std::string name = p.cue->text.substr(0, p.cue->text.find('\n'));
        for (const char *tag : {"<b>", "</b>", "<i>", "</i>", "<u>", "</u>"})
            for (size_t at; (at = name.find(tag)) != std::string::npos;)
                name.erase(at, std::string_view(tag).size());
        if (!run(std::make_unique<core::RenameClip>(insert->clipId(), name)))
            return fail();
    }
    return true;
}

void ImportCaptions::revert(core::Model &model)
{
    for (auto it = m_done.rbegin(); it != m_done.rend(); ++it)
        (*it)->revert(model);
}

} // namespace ustudio::titles::captions
