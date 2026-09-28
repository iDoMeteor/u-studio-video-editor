#include "lottie_check.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace ustudio::titles::lottie {

namespace {

// Where the scanner is: each open object or array, what it is in the
// file's structure, and (for an object) the key being read.
enum class Role
{
    Other,
    Root,  // the top-level object
    Asset, // an object in the root's "assets"
    Layer, // an object in any "layers" (the root's or a precomposition's)
    Font,  // an object in "fonts" / "list"
};

struct Level
{
    bool object = false;
    Role role = Role::Other;
    std::string key;          // an object's current key
    std::string containerKey; // the key this object or array sits under
    int asset = -1;           // the enclosing asset's index, or -1 (the root)
};

struct AssetInfo
{
    std::string id, path;
    bool precomposition = false;
    std::vector<std::string> refs; // refIds its layers use
};

class Scanner
{
  public:
    explicit Scanner(std::string_view text) : m_text(text) {}

    std::expected<Facts, std::string> run()
    {
        skipSpace();
        if (at() != '{')
            return std::unexpected("it isn't a Lottie animation (not a JSON object)");
        if (!value(std::string()))
            return std::unexpected(m_error);
        skipSpace();
        if (m_pos != m_text.size())
            return std::unexpected("it has something after the animation");
        return finish();
    }

  private:
    char at() const
    {
        return m_pos < m_text.size() ? m_text[m_pos] : '\0';
    }

    void skipSpace()
    {
        while (m_pos < m_text.size() &&
               (m_text[m_pos] == ' ' || m_text[m_pos] == '\t' || m_text[m_pos] == '\n' || m_text[m_pos] == '\r'))
            ++m_pos;
    }

    bool fail(const std::string &why)
    {
        if (m_error.empty())
            m_error = why;
        return false;
    }

    bool counted()
    {
        if (++m_values > kMaxValues)
            return fail("it's too complex (over " + std::to_string(kMaxValues) + " values)");
        return true;
    }

    // Any value, sitting under `key` in its parent.
    bool value(const std::string &key)
    {
        if (!counted())
            return false;
        skipSpace();
        switch (at()) {
        case '{':
            return container(true, key);
        case '[':
            return container(false, key);
        case '"': {
            std::string text;
            if (!string(text))
                return false;
            return onString(key, text);
        }
        case 't':
            return literal("true");
        case 'f':
            return literal("false");
        case 'n':
            return literal("null");
        default: {
            double number = 0.0;
            if (!numberValue(number))
                return false;
            return onNumber(key, number);
        }
        }
    }

    bool literal(std::string_view word)
    {
        if (m_text.substr(m_pos, word.size()) != word)
            return fail("it isn't valid JSON (at byte " + std::to_string(m_pos) + ")");
        m_pos += word.size();
        return true;
    }

    bool numberValue(double &out)
    {
        const size_t begin = m_pos;
        if (at() == '-')
            ++m_pos;
        if (!std::isdigit(static_cast<unsigned char>(at())))
            return fail("it isn't valid JSON (at byte " + std::to_string(begin) + ")");
        while (std::isdigit(static_cast<unsigned char>(at())))
            ++m_pos;
        if (at() == '.') {
            ++m_pos;
            if (!std::isdigit(static_cast<unsigned char>(at())))
                return fail("it isn't valid JSON (at byte " + std::to_string(begin) + ")");
            while (std::isdigit(static_cast<unsigned char>(at())))
                ++m_pos;
        }
        if (at() == 'e' || at() == 'E') {
            ++m_pos;
            if (at() == '+' || at() == '-')
                ++m_pos;
            if (!std::isdigit(static_cast<unsigned char>(at())))
                return fail("it isn't valid JSON (at byte " + std::to_string(begin) + ")");
            while (std::isdigit(static_cast<unsigned char>(at())))
                ++m_pos;
        }
        if (m_pos - begin > 64)
            return fail("it has a number that's out of range");
        const std::string text(m_text.substr(begin, m_pos - begin));
        out = std::strtod(text.c_str(), nullptr);
        if (!std::isfinite(out))
            return fail("it has a number that's out of range");
        return true;
    }

    static void appendUtf8(std::string &out, uint32_t cp)
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

    bool hex4(uint32_t &out)
    {
        out = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = at();
            if (!std::isxdigit(static_cast<unsigned char>(c)))
                return fail("it isn't valid JSON (a bad \\u escape)");
            out = out * 16 + static_cast<uint32_t>(std::isdigit(static_cast<unsigned char>(c))
                                                       ? c - '0'
                                                       : std::tolower(static_cast<unsigned char>(c)) - 'a' + 10);
            ++m_pos;
        }
        return true;
    }

    // A string, decoded; its bytes must be UTF-8.
    bool string(std::string &out)
    {
        ++m_pos; // the opening quote
        while (true) {
            if (m_pos >= m_text.size())
                return fail("it isn't valid JSON (a string doesn't end)");
            const auto c = static_cast<unsigned char>(m_text[m_pos]);
            if (c == '"') {
                ++m_pos;
                return true;
            }
            if (c < 0x20)
                return fail("it isn't valid JSON (a control character in a string)");
            if (c == '\\') {
                ++m_pos;
                const char e = at();
                ++m_pos;
                switch (e) {
                case '"':
                case '\\':
                case '/':
                    out += e;
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!hex4(cp))
                        return false;
                    if (cp >= 0xD800 && cp < 0xDC00) {
                        uint32_t low = 0;
                        if (m_text.substr(m_pos, 2) != "\\u")
                            return fail("it isn't valid JSON (a lone surrogate)");
                        m_pos += 2;
                        if (!hex4(low) || low < 0xDC00 || low >= 0xE000)
                            return fail("it isn't valid JSON (a lone surrogate)");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else if (cp >= 0xDC00 && cp < 0xE000) {
                        return fail("it isn't valid JSON (a lone surrogate)");
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default:
                    return fail("it isn't valid JSON (a bad escape)");
                }
                continue;
            }
            // UTF-8: the lead byte says how many continuation bytes follow.
            size_t extra = 0;
            uint32_t cp = 0;
            if (c < 0x80) {
                cp = c;
            } else if ((c & 0xE0) == 0xC0) {
                extra = 1;
                cp = c & 0x1F;
            } else if ((c & 0xF0) == 0xE0) {
                extra = 2;
                cp = c & 0x0F;
            } else if ((c & 0xF8) == 0xF0) {
                extra = 3;
                cp = c & 0x07;
            } else {
                return fail("it isn't UTF-8 text");
            }
            if (m_pos + extra >= m_text.size())
                return fail("it isn't UTF-8 text");
            for (size_t k = 1; k <= extra; ++k) {
                const auto cc = static_cast<unsigned char>(m_text[m_pos + k]);
                if ((cc & 0xC0) != 0x80)
                    return fail("it isn't UTF-8 text");
                cp = (cp << 6) | (cc & 0x3F);
            }
            static constexpr uint32_t kMin[] = {0, 0x80, 0x800, 0x10000};
            if (cp < kMin[extra] || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000))
                return fail("it isn't UTF-8 text");
            out.append(m_text.substr(m_pos, extra + 1));
            m_pos += extra + 1;
        }
    }

    bool container(bool object, const std::string &key)
    {
        if (m_levels.size() >= kMaxDepth)
            return fail("it nests too deeply (over " + std::to_string(kMaxDepth) + " levels)");
        Level level;
        level.object = object;
        level.containerKey = key;
        level.asset = m_levels.empty() ? -1 : m_levels.back().asset;
        if (object) {
            if (m_levels.empty()) {
                level.role = Role::Root;
            } else {
                const Level &parent = m_levels.back();
                const Level *grand = m_levels.size() >= 2 ? &m_levels[m_levels.size() - 2] : nullptr;
                if (!parent.object && parent.containerKey == "assets" && grand && grand->role == Role::Root) {
                    level.role = Role::Asset;
                    if (m_assets.size() >= kMaxAssets)
                        return fail("it has too many assets (over " + std::to_string(kMaxAssets) + ")");
                    m_assets.emplace_back();
                    level.asset = static_cast<int>(m_assets.size()) - 1;
                } else if (!parent.object && parent.containerKey == "layers") {
                    level.role = Role::Layer;
                    if (++m_facts.layers > kMaxLayers)
                        return fail("it has too many layers (over " + std::to_string(kMaxLayers) + ")");
                } else if (!parent.object && parent.containerKey == "list" && grand && grand->object &&
                           grand->containerKey == "fonts") {
                    level.role = Role::Font;
                }
            }
        } else if (key == "layers" && !m_levels.empty()) {
            if (m_levels.back().role == Role::Root)
                m_rootLayers = true;
            else if (m_levels.back().role == Role::Asset)
                m_assets[static_cast<size_t>(m_levels.back().asset)].precomposition = true;
        }
        m_levels.push_back(level);
        ++m_pos; // { or [
        skipSpace();
        const char close = object ? '}' : ']';
        if (at() == close) {
            ++m_pos;
            m_levels.pop_back();
            return true;
        }
        while (true) {
            std::string childKey;
            if (object) {
                skipSpace();
                if (at() != '"')
                    return fail("it isn't valid JSON (at byte " + std::to_string(m_pos) + ")");
                if (!string(childKey))
                    return false;
                skipSpace();
                if (at() != ':')
                    return fail("it isn't valid JSON (at byte " + std::to_string(m_pos) + ")");
                ++m_pos;
                m_levels.back().key = childKey;
            }
            if (!value(object ? childKey : key))
                return false;
            skipSpace();
            if (at() == ',') {
                ++m_pos;
                continue;
            }
            if (at() == close) {
                ++m_pos;
                break;
            }
            return fail("it isn't valid JSON (at byte " + std::to_string(m_pos) + ")");
        }
        m_levels.pop_back();
        return true;
    }

    // A string value under `key` in the innermost object (or an array's
    // item, where `key` is the array's).
    bool onString(const std::string &key, const std::string &text)
    {
        if (m_levels.empty() || !m_levels.back().object)
            return true;
        const Level &level = m_levels.back();
        // An expression: a script on an animated property.
        if (key == "x")
            return fail("it uses expressions (scripts), which aren't supported; bake them into keyframes "
                        "when exporting");
        if (level.role == Role::Root && key == "v")
            m_version = true;
        if (level.role == Role::Font && key == "fPath" && !text.empty())
            return fail("it loads a font from outside the file");
        if (level.role == Role::Asset) {
            AssetInfo &asset = m_assets[static_cast<size_t>(level.asset)];
            if (key == "id")
                asset.id = text;
            else if (key == "p")
                asset.path = text;
        }
        if (level.role == Role::Layer && key == "refId") {
            if (level.asset >= 0)
                m_assets[static_cast<size_t>(level.asset)].refs.push_back(text);
        }
        return true;
    }

    bool onNumber(const std::string &key, double number)
    {
        if (m_levels.empty() || !m_levels.back().object)
            return true;
        const Level &level = m_levels.back();
        if (level.role == Role::Root) {
            if (key == "fr")
                m_fr = number;
            else if (key == "ip")
                m_ip = number;
            else if (key == "op")
                m_op = number;
            else if (key == "w")
                m_w = number;
            else if (key == "h")
                m_h = number;
        }
        if (level.role == Role::Layer && key == "ty" && number == 5.0)
            m_facts.hasText = true;
        return true;
    }

    std::expected<Facts, std::string> finish()
    {
        if (!m_version || !m_fr || !m_ip || !m_op || !m_w || !m_h || !m_rootLayers)
            return std::unexpected("it isn't a Lottie animation (its header is missing)");
        if (*m_fr < 1.0 || *m_fr > 120.0)
            return std::unexpected("its frame rate is out of range (1 to 120 fps)");
        if (!(*m_ip < *m_op))
            return std::unexpected("it ends before it starts");
        if ((*m_op - *m_ip) / *m_fr > kMaxSeconds)
            return std::unexpected("it's over ten minutes long");
        for (double side : {*m_w, *m_h})
            if (side < 1.0 || side > kMaxSide || side != std::floor(side))
                return std::unexpected("its size is out of range (1 to " + std::to_string(kMaxSide) + " pixels)");
        for (const AssetInfo &asset : m_assets)
            if (!asset.precomposition && !asset.path.empty())
                if (auto bad = checkImage(asset.path))
                    return std::unexpected(*bad);
        if (auto bad = checkPrecompositions())
            return std::unexpected(*bad);
        m_facts.fr = *m_fr;
        m_facts.ip = *m_ip;
        m_facts.op = *m_op;
        m_facts.width = static_cast<int>(*m_w);
        m_facts.height = static_cast<int>(*m_h);
        return m_facts;
    }

    // An image asset: an embedded PNG or JPEG of sane size, nothing else.
    static std::optional<std::string> checkImage(const std::string &path)
    {
        std::string_view data = path;
        bool png = false;
        if (data.starts_with("data:image/png;base64,")) {
            png = true;
            data.remove_prefix(22);
        } else if (data.starts_with("data:image/jpeg;base64,")) {
            data.remove_prefix(23);
        } else if (data.starts_with("data:image/jpg;base64,")) {
            data.remove_prefix(22);
        } else {
            return "it uses a picture from outside the file (only embedded PNG and JPEG pictures are allowed)";
        }
        std::string bytes;
        if (!base64(data, bytes))
            return "an embedded picture isn't valid base64";
        if (bytes.size() > kMaxImageBytes)
            return "an embedded picture is over 16 MB";
        int width = 0, height = 0;
        if (png) {
            if (bytes.size() < 24 || bytes.compare(0, 8, "\x89PNG\r\n\x1a\n") != 0 || bytes.compare(12, 4, "IHDR") != 0)
                return "an embedded picture isn't the PNG it says it is";
            const auto u32 = [&](size_t at) {
                return static_cast<int>((static_cast<uint32_t>(static_cast<unsigned char>(bytes[at])) << 24) |
                                        (static_cast<uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 16) |
                                        (static_cast<uint32_t>(static_cast<unsigned char>(bytes[at + 2])) << 8) |
                                        static_cast<uint32_t>(static_cast<unsigned char>(bytes[at + 3])));
            };
            width = u32(16);
            height = u32(20);
        } else {
            if (bytes.size() < 4 || bytes.compare(0, 3, "\xff\xd8\xff") != 0)
                return "an embedded picture isn't the JPEG it says it is";
            // The frame header (SOF0..SOF15, not DHT, JPG or DAC) has the size.
            size_t at = 2;
            while (at + 9 < bytes.size()) {
                if (static_cast<unsigned char>(bytes[at]) != 0xFF)
                    return "an embedded picture isn't the JPEG it says it is";
                const auto marker = static_cast<unsigned char>(bytes[at + 1]);
                const size_t length = (static_cast<size_t>(static_cast<unsigned char>(bytes[at + 2])) << 8) |
                                      static_cast<unsigned char>(bytes[at + 3]);
                if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
                    height =
                        (static_cast<unsigned char>(bytes[at + 5]) << 8) | static_cast<unsigned char>(bytes[at + 6]);
                    width =
                        (static_cast<unsigned char>(bytes[at + 7]) << 8) | static_cast<unsigned char>(bytes[at + 8]);
                    break;
                }
                if (length < 2)
                    return "an embedded picture isn't the JPEG it says it is";
                at += 2 + length;
            }
        }
        if (width < 1 || height < 1 || width > kMaxSide || height > kMaxSide)
            return "an embedded picture's size is out of range (1 to " + std::to_string(kMaxSide) + " pixels)";
        return std::nullopt;
    }

    static bool base64(std::string_view in, std::string &out)
    {
        uint32_t buffer = 0;
        int bits = 0;
        size_t padding = 0;
        for (char c : in) {
            int v = -1;
            if (c >= 'A' && c <= 'Z')
                v = c - 'A';
            else if (c >= 'a' && c <= 'z')
                v = c - 'a' + 26;
            else if (c >= '0' && c <= '9')
                v = c - '0' + 52;
            else if (c == '+')
                v = 62;
            else if (c == '/')
                v = 63;
            else if (c == '=') {
                ++padding;
                continue;
            } else {
                return false;
            }
            if (padding)
                return false; // data after padding
            buffer = (buffer << 6) | static_cast<uint32_t>(v);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out += static_cast<char>((buffer >> bits) & 0xFF);
            }
            if (out.size() > kMaxImageBytes)
                return true; // too big: the caller says so
        }
        return padding <= 2;
    }

    // No precomposition may use itself, directly or through others.
    std::optional<std::string> checkPrecompositions() const
    {
        std::map<std::string, size_t> byId;
        for (size_t i = 0; i < m_assets.size(); ++i)
            if (m_assets[i].precomposition)
                byId[m_assets[i].id] = i;
        std::vector<int> state(m_assets.size(), 0); // 0 new, 1 on the path, 2 done
        // Iterative depth-first search: a hostile file can't blow the stack.
        for (size_t start = 0; start < m_assets.size(); ++start) {
            if (state[start] != 0 || !m_assets[start].precomposition)
                continue;
            std::vector<std::pair<size_t, size_t>> stack = {{start, 0}};
            state[start] = 1;
            while (!stack.empty()) {
                auto &[node, next] = stack.back();
                if (next < m_assets[node].refs.size()) {
                    auto it = byId.find(m_assets[node].refs[next++]);
                    if (it == byId.end())
                        continue;
                    if (state[it->second] == 1)
                        return "a precomposition in it uses itself";
                    if (state[it->second] == 0) {
                        state[it->second] = 1;
                        stack.push_back({it->second, 0});
                    }
                } else {
                    state[node] = 2;
                    stack.pop_back();
                }
            }
        }
        return std::nullopt;
    }

    std::string_view m_text;
    size_t m_pos = 0, m_values = 0;
    std::string m_error;
    std::vector<Level> m_levels;
    std::vector<AssetInfo> m_assets;
    Facts m_facts;
    bool m_version = false, m_rootLayers = false;
    std::optional<double> m_fr, m_ip, m_op, m_w, m_h;
};

} // namespace

double frameAt(double titleFrame, int fpsNum, int fpsDen, const Facts &facts, double speed, bool loop)
{
    const double length = facts.op - facts.ip;
    if (fpsNum <= 0 || fpsDen <= 0 || length <= 0.0 || titleFrame <= 0.0)
        return facts.ip;
    const double elapsed = titleFrame * fpsDen * facts.fr * speed / fpsNum;
    const double at = loop ? std::fmod(elapsed, length) : std::min(elapsed, std::max(0.0, length - 1.0));
    return facts.ip + at;
}

std::expected<Facts, std::string> check(std::string_view json)
{
    if (json.size() > kMaxBytes)
        return std::unexpected("it's over " + std::to_string(kMaxBytes >> 20) + " MB");
    return Scanner(json).run();
}

} // namespace ustudio::titles::lottie
