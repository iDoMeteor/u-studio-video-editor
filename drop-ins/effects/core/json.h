#pragma once

// A small JSON value, reader and writer for the effects drop-in's own data:
// curated overlays, the registry cache and the probe's result line (doc 15).
// Only what those need: objects keep their key order, numbers are doubles,
// and \u escapes outside the Basic Multilingual Plane are joined from their
// surrogate pairs. Not a general-purpose library; nothing else may grow a
// dependency on it (a second user is the moment to move it to src/core).

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ustudio::effects {

class Json
{
  public:
    using Array = std::vector<Json>;
    using Object = std::vector<std::pair<std::string, Json>>;

    Json() = default; // null
    Json(std::nullptr_t) {}
    Json(bool value) : m_value(value) {}
    Json(double value) : m_value(value) {}
    Json(int value) : m_value(static_cast<double>(value)) {}
    Json(const char *value) : m_value(std::string(value)) {}
    Json(std::string value) : m_value(std::move(value)) {}
    Json(Array value) : m_value(std::move(value)) {}
    Json(Object value) : m_value(std::move(value)) {}

    bool isNull() const
    {
        return std::holds_alternative<std::monostate>(m_value);
    }
    bool isBool() const
    {
        return std::holds_alternative<bool>(m_value);
    }
    bool isNumber() const
    {
        return std::holds_alternative<double>(m_value);
    }
    bool isString() const
    {
        return std::holds_alternative<std::string>(m_value);
    }
    bool isArray() const
    {
        return std::holds_alternative<Array>(m_value);
    }
    bool isObject() const
    {
        return std::holds_alternative<Object>(m_value);
    }

    // The value, or `fallback` when it is of another type.
    bool asBool(bool fallback = false) const;
    double asNumber(double fallback = 0.0) const;
    std::string asString(const std::string &fallback = {}) const;
    const Array &asArray() const;   // empty when not an array
    const Object &asObject() const; // empty when not an object

    // An object's member; null (a shared static) when absent or not an object.
    const Json &operator[](std::string_view key) const;
    bool has(std::string_view key) const;
    // Adds or replaces an object's member (a null value becomes an object).
    void set(std::string key, Json value);
    // Appends to an array (a null value becomes an array).
    void push(Json value);

    bool operator==(const Json &) const = default;

  private:
    std::variant<std::monostate, bool, double, std::string, Array, Object> m_value;
};

// Nullopt on any syntax error, with `error` (if given) saying where.
std::optional<Json> parseJson(std::string_view text, std::string *error = nullptr);
// Compact, on one line (the probe's output is one JSON line per result).
std::string toJson(const Json &value);
// A string's JSON literal, quotes included.
std::string jsonString(std::string_view text);

} // namespace ustudio::effects
