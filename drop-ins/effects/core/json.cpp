#include "core/json.h"

#include "core/model/animation.h"

#include <charconv>
#include <cmath>
#include <cstdint>

namespace ustudio::effects {

namespace {

const Json kNull;
const Json::Array kEmptyArray;
const Json::Object kEmptyObject;

class Parser
{
  public:
    explicit Parser(std::string_view text) : m_text(text) {}

    std::optional<Json> document(std::string *error)
    {
        std::optional<Json> value = parseValue(0);
        skipSpace();
        if (value && m_pos != m_text.size())
            value = fail("trailing characters");
        if (!value && error)
            *error = m_error + " at offset " + std::to_string(m_pos);
        return value;
    }

  private:
    // Deep enough for any file we write; a hostile file can't recurse the
    // stack away.
    static constexpr int kMaxDepth = 64;

    std::optional<Json> fail(const char *what)
    {
        if (m_error.empty())
            m_error = what;
        return std::nullopt;
    }

    void skipSpace()
    {
        while (m_pos < m_text.size() &&
               (m_text[m_pos] == ' ' || m_text[m_pos] == '\t' || m_text[m_pos] == '\n' || m_text[m_pos] == '\r'))
            ++m_pos;
    }

    bool consume(std::string_view word)
    {
        if (m_text.substr(m_pos, word.size()) != word)
            return false;
        m_pos += word.size();
        return true;
    }

    std::optional<Json> parseValue(int depth)
    {
        if (depth > kMaxDepth)
            return fail("nested too deeply");
        skipSpace();
        if (m_pos >= m_text.size())
            return fail("unexpected end");
        const char c = m_text[m_pos];
        if (c == '{')
            return parseObject(depth);
        if (c == '[')
            return parseArray(depth);
        if (c == '"') {
            std::optional<std::string> text = parseString();
            if (!text)
                return std::nullopt;
            return Json(std::move(*text));
        }
        if (consume("true"))
            return Json(true);
        if (consume("false"))
            return Json(false);
        if (consume("null"))
            return Json();
        if (c == '-' || (c >= '0' && c <= '9'))
            return parseNumber();
        return fail("unexpected character");
    }

    std::optional<Json> parseNumber()
    {
        const size_t start = m_pos;
        if (m_text[m_pos] == '-')
            ++m_pos;
        auto digits = [&] {
            const size_t from = m_pos;
            while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9')
                ++m_pos;
            return m_pos > from;
        };
        if (!digits())
            return fail("bad number");
        if (m_pos < m_text.size() && m_text[m_pos] == '.') {
            ++m_pos;
            if (!digits())
                return fail("bad number");
        }
        if (m_pos < m_text.size() && (m_text[m_pos] == 'e' || m_text[m_pos] == 'E')) {
            ++m_pos;
            if (m_pos < m_text.size() && (m_text[m_pos] == '+' || m_text[m_pos] == '-'))
                ++m_pos;
            if (!digits())
                return fail("bad number");
        }
        // from_chars, not strtod: strtod follows LC_NUMERIC (reader.cpp's toDouble()).
        double value = 0.0;
        const auto result = std::from_chars(m_text.data() + start, m_text.data() + m_pos, value);
        if (result.ec != std::errc() || result.ptr != m_text.data() + m_pos)
            return fail("bad number");
        return Json(value);
    }

    static void appendUtf8(std::string &out, uint32_t code)
    {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    std::optional<uint32_t> hex4()
    {
        if (m_pos + 4 > m_text.size())
            return std::nullopt;
        uint32_t code = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = m_text[m_pos++];
            code <<= 4;
            if (h >= '0' && h <= '9')
                code |= static_cast<uint32_t>(h - '0');
            else if (h >= 'a' && h <= 'f')
                code |= static_cast<uint32_t>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F')
                code |= static_cast<uint32_t>(h - 'A' + 10);
            else
                return std::nullopt;
        }
        return code;
    }

    std::optional<std::string> parseString()
    {
        ++m_pos; // the opening quote
        std::string out;
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos++];
            if (c == '"')
                return out;
            if (static_cast<unsigned char>(c) < 0x20) {
                fail("control character in string");
                return std::nullopt;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (m_pos >= m_text.size())
                break;
            const char e = m_text[m_pos++];
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
                std::optional<uint32_t> code = hex4();
                if (!code) {
                    fail("bad \\u escape");
                    return std::nullopt;
                }
                if (*code >= 0xD800 && *code < 0xDC00 && consume("\\u")) {
                    std::optional<uint32_t> low = hex4();
                    if (!low || *low < 0xDC00 || *low >= 0xE000) {
                        fail("bad surrogate pair");
                        return std::nullopt;
                    }
                    *code = 0x10000 + ((*code - 0xD800) << 10) + (*low - 0xDC00);
                }
                appendUtf8(out, *code);
                break;
            }
            default:
                fail("bad escape");
                return std::nullopt;
            }
        }
        fail("unterminated string");
        return std::nullopt;
    }

    std::optional<Json> parseArray(int depth)
    {
        ++m_pos;
        Json::Array items;
        skipSpace();
        if (consume("]"))
            return Json(std::move(items));
        while (true) {
            std::optional<Json> item = parseValue(depth + 1);
            if (!item)
                return std::nullopt;
            items.push_back(std::move(*item));
            skipSpace();
            if (consume("]"))
                return Json(std::move(items));
            if (!consume(","))
                return fail("expected ',' or ']'");
        }
    }

    std::optional<Json> parseObject(int depth)
    {
        ++m_pos;
        Json::Object members;
        skipSpace();
        if (consume("}"))
            return Json(std::move(members));
        while (true) {
            skipSpace();
            if (m_pos >= m_text.size() || m_text[m_pos] != '"')
                return fail("expected a key");
            std::optional<std::string> key = parseString();
            if (!key)
                return std::nullopt;
            skipSpace();
            if (!consume(":"))
                return fail("expected ':'");
            std::optional<Json> value = parseValue(depth + 1);
            if (!value)
                return std::nullopt;
            members.emplace_back(std::move(*key), std::move(*value));
            skipSpace();
            if (consume("}"))
                return Json(std::move(members));
            if (!consume(","))
                return fail("expected ',' or '}'");
        }
    }

    std::string_view m_text;
    size_t m_pos = 0;
    std::string m_error;
};

void write(std::string &out, const Json &value)
{
    if (value.isNull()) {
        out += "null";
    } else if (value.isBool()) {
        out += value.asBool() ? "true" : "false";
    } else if (value.isNumber()) {
        const double number = value.asNumber();
        out += std::isfinite(number) ? core::formatDouble(number) : "null";
    } else if (value.isString()) {
        out += jsonString(value.asString());
    } else if (value.isArray()) {
        out += '[';
        bool first = true;
        for (const Json &item : value.asArray()) {
            if (!first)
                out += ',';
            first = false;
            write(out, item);
        }
        out += ']';
    } else {
        out += '{';
        bool first = true;
        for (const auto &[key, item] : value.asObject()) {
            if (!first)
                out += ',';
            first = false;
            out += jsonString(key);
            out += ':';
            write(out, item);
        }
        out += '}';
    }
}

} // namespace

bool Json::asBool(bool fallback) const
{
    const bool *value = std::get_if<bool>(&m_value);
    return value ? *value : fallback;
}

double Json::asNumber(double fallback) const
{
    const double *value = std::get_if<double>(&m_value);
    return value ? *value : fallback;
}

std::string Json::asString(const std::string &fallback) const
{
    const std::string *value = std::get_if<std::string>(&m_value);
    return value ? *value : fallback;
}

const Json::Array &Json::asArray() const
{
    const Array *value = std::get_if<Array>(&m_value);
    return value ? *value : kEmptyArray;
}

const Json::Object &Json::asObject() const
{
    const Object *value = std::get_if<Object>(&m_value);
    return value ? *value : kEmptyObject;
}

const Json &Json::operator[](std::string_view key) const
{
    for (const auto &[name, value] : asObject())
        if (name == key)
            return value;
    return kNull;
}

bool Json::has(std::string_view key) const
{
    for (const auto &[name, value] : asObject())
        if (name == key)
            return true;
    return false;
}

void Json::set(std::string key, Json value)
{
    if (isNull())
        m_value = Object{};
    Object *members = std::get_if<Object>(&m_value);
    if (!members)
        return;
    for (auto &[name, existing] : *members) {
        if (name == key) {
            existing = std::move(value);
            return;
        }
    }
    members->emplace_back(std::move(key), std::move(value));
}

void Json::push(Json value)
{
    if (isNull())
        m_value = Array{};
    if (Array *items = std::get_if<Array>(&m_value))
        items->push_back(std::move(value));
}

std::optional<Json> parseJson(std::string_view text, std::string *error)
{
    return Parser(text).document(error);
}

std::string toJson(const Json &value)
{
    std::string out;
    write(out, value);
    return out;
}

std::string jsonString(std::string_view text)
{
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                static const char *kHex = "0123456789abcdef";
                out += "\\u00";
                out += kHex[(c >> 4) & 0xF];
                out += kHex[c & 0xF];
            } else {
                out += c;
            }
        }
    }
    out += '"';
    return out;
}

} // namespace ustudio::effects
