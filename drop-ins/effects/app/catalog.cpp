#include "app/catalog.h"

#include "engine/effects_extension.h"

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

} // namespace ustudio::effects
