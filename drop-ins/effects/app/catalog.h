#pragma once

// What the effects UI knows about the effects on offer (main thread): the
// registry and the health scan's results, as they arrive from the scan's
// thread (app/health_scan.h). The Rack and the Browser read it and redraw
// on `changed`.

#include "core/health.h"
#include "core/model/signal.h"
#include "engine/registry.h"

#include <memory>
#include <string>
#include <vector>

namespace ustudio::effects {

class Catalog
{
  public:
    void setRegistry(std::shared_ptr<const EffectRegistry> registry);
    void setHealth(const std::string &service, const HealthRecord &record);

    bool ready() const
    {
        return m_registry != nullptr;
    }
    // Null until the scan delivers the registry, or for a service MLT
    // doesn't offer here.
    const EffectDescriptor *find(const std::string &service) const;
    // Offered in the Browser: not plumbing, not unstable, not quarantined
    // (unless `showUnstable`), in the registry's order.
    std::vector<const EffectDescriptor *> offered(bool showUnstable = false) const;
    std::optional<HealthRecord> health(const std::string &service) const;
    bool usable(const std::string &service) const;

    core::Signal<> changed;

  private:
    std::shared_ptr<const EffectRegistry> m_registry;
    HealthFile m_health;
};

} // namespace ustudio::effects
