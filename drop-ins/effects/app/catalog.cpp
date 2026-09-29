#include "app/catalog.h"

#include <filesystem>

#include "core/looks.h"
#include "engine/effects_extension.h"

#include <charconv>

namespace ustudio::effects {

void Catalog::setRegistry(std::shared_ptr<const EffectRegistry> registry)
{
    m_registry = std::move(registry);
    changed.emit();
}

void Catalog::setHealth(const std::string &service, const HealthRecord &record)
{
    m_health.records[service] = record;
    healthChanged.emit(service);
}

const EffectDescriptor *Catalog::find(const std::string &service) const
{
    return m_registry ? m_registry->find(service) : nullptr;
}

std::vector<const EffectDescriptor *> Catalog::offered(bool showUnstable) const
{
    std::vector<const EffectDescriptor *> out;
    if (!m_registry)
        return out;
    for (const EffectDescriptor &d : m_registry->descriptors()) {
        if (d.hidden)
            continue;
        if (!showUnstable && !usable(d.service))
            continue;
        out.push_back(&d);
    }
    return out;
}

std::optional<HealthRecord> Catalog::health(const std::string &service) const
{
    return m_health.find(service);
}

bool Catalog::usable(const std::string &service) const
{
    const EffectDescriptor *d = find(service);
    if (d && !d->unstable.empty())
        return false;
    return !m_health.quarantined(service) && !isQuarantined(service);
}

namespace {

const core::Look *lookFor(const Catalog &catalog, const core::Model &model, const std::string &item)
{
    static constexpr std::string_view kBrand = "look:brand:", kProject = "look:project:";
    auto number = [](std::string_view text) -> std::optional<uint64_t> {
        uint64_t value = 0;
        auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        return ec == std::errc() && end == text.data() + text.size() ? std::optional(value) : std::nullopt;
    };
    if (item.starts_with(kBrand)) {
        const std::optional<uint64_t> index = number(std::string_view(item).substr(kBrand.size()));
        return index && *index < catalog.brandLooks().size() ? &catalog.brandLooks()[*index] : nullptr;
    }
    if (item.starts_with(kProject)) {
        const std::optional<uint64_t> id = number(std::string_view(item).substr(kProject.size()));
        for (const core::Look &look : model.project().looks)
            if (id && look.id.value == *id)
                return &look;
    }
    return nullptr;
}

} // namespace

std::vector<core::Effect> Catalog::effectsFor(const core::Model &model, const std::string &item) const
{
    if (const core::Look *look = lookFor(*this, model, item))
        return effectsOf(*look);
    if (item.starts_with(kLutPrefix)) {
        const EffectDescriptor *d = find("avfilter.lut3d");
        if (!d)
            return {};
        core::Effect effect = makeEffect(*d);
        auto set = [&](const std::string &name, const std::string &value) {
            for (core::Param &p : effect.params)
                if (p.name == name) {
                    p.value = value;
                    return;
                }
            effect.params.push_back({name, value, {}});
        };
        set("av.file", item.substr(kLutPrefix.size()));
        set("av.interp", "tetrahedral");
        return {effect};
    }
    if (const EffectDescriptor *d = find(item))
        return {makeEffect(*d)};
    return {};
}

std::string Catalog::nameOf(const core::Model &model, const std::string &item) const
{
    if (const core::Look *look = lookFor(*this, model, item))
        return look->name;
    if (item.starts_with(kLutPrefix))
        return std::filesystem::path(item.substr(kLutPrefix.size())).stem().string();
    const EffectDescriptor *d = find(item);
    return d ? d->name : item;
}

std::vector<std::string> Catalog::servicesOf(const core::Model &model, const std::string &item) const
{
    std::vector<std::string> services;
    for (const core::Effect &effect : effectsFor(model, item))
        services.push_back(effect.service);
    return services;
}

} // namespace ustudio::effects
