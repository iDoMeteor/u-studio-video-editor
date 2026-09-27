#include "core/media/image_sequence.h"

#include "core/media/utf8_path.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <set>
#include <vector>

namespace ustudio::core {

namespace {

struct NameParts
{
    std::string prefix, digits, suffix;
};

std::optional<NameParts> split(const std::string &name)
{
    size_t end = name.size();
    while (end > 0 && !std::isdigit(static_cast<unsigned char>(name[end - 1])))
        --end;
    if (end == 0)
        return std::nullopt;
    size_t start = end;
    while (start > 0 && std::isdigit(static_cast<unsigned char>(name[start - 1])))
        --start;
    if (end - start > 9)
        return std::nullopt; // not a frame number (a timestamp, say)
    return NameParts{name.substr(0, start), name.substr(start, end - start), name.substr(end)};
}

// A literal '%' in a file name would read as a printf directive.
std::string escaped(const std::string &text)
{
    std::string out;
    for (char c : text)
        out += c == '%' ? std::string("%%") : std::string(1, c);
    return out;
}

} // namespace

std::optional<ImageSequence> findImageSequence(const std::string &pickedFile)
{
    const std::filesystem::path picked = pathFromUtf8(pickedFile);
    const std::optional<NameParts> parts = split(utf8String(picked.filename()));
    if (!parts)
        return std::nullopt;
    const int pickedNumber = std::stoi(parts->digits);
    std::set<int> numbers;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(picked.parent_path(), ec)) {
        if (!entry.is_regular_file(ec))
            continue;
        const std::optional<NameParts> other = split(utf8String(entry.path().filename()));
        if (other && other->prefix == parts->prefix && other->suffix == parts->suffix &&
            other->digits.size() == parts->digits.size())
            numbers.insert(std::stoi(other->digits));
    }
    if (!numbers.contains(pickedNumber))
        return std::nullopt;
    int first = pickedNumber, last = pickedNumber;
    while (numbers.contains(first - 1))
        --first;
    while (numbers.contains(last + 1))
        ++last;
    if (last == first)
        return std::nullopt;
    const std::string width = std::to_string(parts->digits.size());
    ImageSequence sequence;
    sequence.pattern = utf8String(picked.parent_path() /
                                  pathFromUtf8(escaped(parts->prefix) + "%0" + width + "d" + escaped(parts->suffix)));
    sequence.begin = first;
    sequence.count = last - first + 1;
    char range[48];
    std::snprintf(range, sizeof range, "[%0*d-%0*d]", static_cast<int>(parts->digits.size()), first,
                  static_cast<int>(parts->digits.size()), last);
    sequence.displayName = parts->prefix + range + parts->suffix;
    return sequence;
}

std::string imageSequenceFile(const std::string &pattern, int number)
{
    // The pattern has exactly one %0Nd (findImageSequence() escapes any
    // other '%'), so formatting it is safe.
    std::vector<char> buffer(pattern.size() + 32);
    std::snprintf(buffer.data(), buffer.size(), pattern.c_str(), number);
    return buffer.data();
}

} // namespace ustudio::core
