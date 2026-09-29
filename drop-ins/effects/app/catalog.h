#pragma once

// What the effects UI knows about the effects on offer (main thread): the
// registry and the health scan's results, as they arrive from the scan's
// thread (app/health_scan.h). The Rack and the Browser read it and redraw
// on `changed`.

#include "core/health.h"
#include "core/model/model.h"
#include "core/model/signal.h"
#include "engine/plugins.h"
#include "engine/registry.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ustudio::effects {

// A LUT library item's prefix (Catalog::effectsFor()).
inline constexpr std::string_view kLutPrefix = "lut:";

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

    // The registry arrived (everything may differ).
    core::Signal<> changed;

    // Brand Looks, shipped with the drop-in (data/looks/brand.json).
    void setBrandLooks(std::vector<core::Look> looks)
    {
        m_brandLooks = std::move(looks);
    }
    const std::vector<core::Look> &brandLooks() const
    {
        return m_brandLooks;
    }

    // What a Browser tile carries, dragged or applied: an effect's service,
    // "look:brand:<index>", "look:project:<id>" or "lut:<path>" (a .cube
    // file in the LUT library, as avfilter.lut3d). The effects it adds (new,
    // at their defaults or the look's settings), or none when it names
    // nothing this install can play.
    std::vector<core::Effect> effectsFor(const core::Model &model, const std::string &item) const;
    std::string nameOf(const core::Model &model, const std::string &item) const;
    // The services an item runs (all must have passed the check to audition).
    std::vector<std::string> servicesOf(const core::Model &model, const std::string &item) const;

    // Copy and paste of a stack (the Rack's menu): process-wide.
    std::vector<core::Effect> clipboard;
    // One service's health result arrived (its badge, whether it's usable).
    core::Signal<const std::string &> healthChanged;

    // VST2 and OpenFX: offered only when chosen (as loaded at start-up).
    ExperimentalFamilies experimental;
    // Whether LSP Plugins' LADSPA set is installed (doc 15: recommended for
    // audio, never required); the Browser suggests it when it isn't.
    bool lspInstalled = true;

    // The adjustment block the FX lane selected (the shell's selection has
    // no blocks yet): the Rack edits its effects. Null when none.
    std::optional<core::AdjustmentBlockId> selectedBlock;
    // After selectedBlock changes. Process-wide, like the clipboard: with
    // two windows, a block selected in one shows in both Racks.
    core::Signal<> blockSelected;

  private:
    std::shared_ptr<const EffectRegistry> m_registry;
    HealthFile m_health;
    std::vector<core::Look> m_brandLooks;
};

} // namespace ustudio::effects
