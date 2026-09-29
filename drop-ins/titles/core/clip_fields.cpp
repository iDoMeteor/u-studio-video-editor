#include "clip_fields.h"

#include "captions.h"

#include <algorithm>
#include <variant>

namespace ustudio::titles {

namespace {

constexpr std::string_view kPrefix = "field.";

bool isField(const core::Param &param)
{
    return param.name.starts_with(kPrefix);
}

} // namespace

std::map<std::string, std::string> clipFieldValues(const core::Clip &clip)
{
    std::map<std::string, std::string> values;
    for (const core::Param &param : clip.sourceParams)
        if (isField(param))
            if (const auto *value = std::get_if<std::string>(&param.value))
                values.emplace(param.name.substr(kPrefix.size()), *value);
    return values;
}

SetClipFields::SetClipFields(core::ClipId clip, std::map<std::string, std::string> values, uint64_t gesture)
    : m_clip(clip), m_values(std::move(values)), m_gesture(gesture)
{}

bool SetClipFields::apply(core::Model &model)
{
    if (!model.hasClip(m_clip))
        return false;
    m_old = model.clip(m_clip).sourceParams;
    m_new.clear();
    std::copy_if(m_old.begin(), m_old.end(), std::back_inserter(m_new),
                 [](const core::Param &param) { return !isField(param); });
    for (const auto &[name, value] : m_values)
        m_new.push_back({std::string(kPrefix) + name, value, {}});
    // A caption's words changed: its clip's name follows their first line,
    // while the name is still the one the old words gave it.
    const auto captionIn = [](const std::vector<core::Param> &params) -> std::string {
        for (const core::Param &param : params)
            if (param.name == std::string(kPrefix) + "caption")
                if (const auto *text = std::get_if<std::string>(&param.value))
                    return *text;
        return {};
    };
    const std::string before = captionIn(m_old), after = captionIn(m_new);
    m_oldName = m_newName = model.clip(m_clip).name;
    if (before != after && !before.empty() && m_oldName == captions::captionName(before) &&
        !captions::captionName(after).empty())
        m_newName = captions::captionName(after);
    model.setClipSourceParams(m_clip, m_new);
    if (m_newName != m_oldName)
        model.setClipName(m_clip, m_newName);
    return true;
}

void SetClipFields::revert(core::Model &model)
{
    model.setClipSourceParams(m_clip, m_old);
    if (m_newName != m_oldName)
        model.setClipName(m_clip, m_oldName);
}

bool SetClipFields::mergeWith(const core::Command &next)
{
    const auto *other = dynamic_cast<const SetClipFields *>(&next);
    if (!other || m_gesture == 0 || other->m_gesture != m_gesture || other->m_clip != m_clip)
        return false;
    // `next` is applied already; keep our m_old.
    m_values = other->m_values;
    m_new = other->m_new;
    m_newName = other->m_newName;
    return true;
}

bool SetClipFields::isNoOp() const
{
    return m_new == m_old && m_newName == m_oldName;
}

} // namespace ustudio::titles
