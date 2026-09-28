#include "engine/effects_extension.h"

#include "core/descriptor.h"
#include "core/log.h"
#include "core/model/effect_native.h"

#include <mutex>
#include <unordered_map>

namespace ustudio::effects {

namespace {

std::mutex &quarantineMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::set<std::string> &quarantined()
{
    static std::set<std::string> services;
    return services;
}

// cairoblend when frei0r is loaded (the drop-in's dependency, ADR-014),
// else affine (core/model/effect_native.h has the measurements).
core::MixTransition mixTransition()
{
    static const core::MixTransition transition = [] {
        // The repository registers services as data, not strings: ask
        // whether the name exists, not for its value.
        Mlt::Properties transitions(mlt_repository_transitions(mlt_factory_repository()));
        const bool cairoblend = transitions.is_valid() && transitions.property_exists("frei0r.cairoblend");
        if (!cairoblend)
            core::Log::info("[effects] frei0r.cairoblend not available: effect mix composites with affine");
        return cairoblend ? core::MixTransition::Cairoblend : core::MixTransition::Affine;
    }();
    return transition;
}

class EffectsExtension : public engine::EngineExtension
{
  public:
    std::string name() const override
    {
        return kOwner;
    }

    void beginBuild() override
    {
        m_attached.clear();
    }

    void decorateCut(Mlt::Producer &cut, const engine::CutContext &context) override
    {
        attach(cut, context.clip.effects, context.offset, context.length, context.profile, &cut);
    }

    void decoratePlaylist(Mlt::Playlist &playlist, const core::Model &model, const core::Track &track,
                          Mlt::Profile &profile) override
    {
        attach(playlist, track.effects, 0, sequenceLength(model), profile);
    }

    void decorateTractor(Mlt::Tractor &tractor, const core::Model &model, Mlt::Profile &profile) override
    {
        attach(tractor, model.sequence().effects, 0, sequenceLength(model), profile);
    }

    bool applyInPlace(const engine::ParamChange &change) override
    {
        auto found = m_attached.find(change.effect.id.value);
        if (found == m_attached.end())
            return false; // not ours, or not attached (quarantined, disabled drop-in)
        // Same filters, new values: set them. A change of shape (a mix
        // leaving or reaching a constant 1) needs different filters: rebuild.
        for (const Attached &attached : found->second) {
            const std::vector<core::NativeFilter> natives =
                core::nativeFilters(change.effect, attached.offset, attached.length, mixTransition());
            if (natives.size() != attached.filters.size())
                return false;
        }
        for (const Attached &attached : found->second) {
            const std::vector<core::NativeFilter> natives =
                core::nativeFilters(change.effect, attached.offset, attached.length, mixTransition());
            for (size_t i = 0; i < natives.size(); ++i)
                for (const auto &[name, value] : natives[i].properties)
                    attached.filters[i]->set(name.c_str(), value.c_str());
        }
        ++extensionStats().inPlace;
        return true;
    }

  private:
    struct Attached
    {
        std::vector<std::shared_ptr<Mlt::Filter>> filters; // nativeFilters() order
        core::FrameIndex offset, length;
    };

    static core::FrameIndex sequenceLength(const core::Model &model)
    {
        return std::max<core::FrameIndex>(model.sequence().length(), 1);
    }

    // `cut` when `service` is a clip's cut (engine::attachToCut()).
    void attach(Mlt::Service &service, const std::vector<core::Effect> &effects, core::FrameIndex offset,
                core::FrameIndex length, Mlt::Profile &profile, Mlt::Producer *cut = nullptr)
    {
        for (const core::Effect &effect : effects) {
            if (effect.owner != kOwner) {
                ++extensionStats().skipped;
                continue;
            }
            if (isQuarantined(effect.service)) {
                warnOnce(effect.service, "quarantined by the health scan; playing without it");
                ++extensionStats().skipped;
                continue;
            }
            Attached attached{{}, offset, length};
            bool valid = true;
            for (const core::NativeFilter &native : core::nativeFilters(effect, offset, length, mixTransition())) {
                auto filter = std::make_shared<Mlt::Filter>(profile, native.service.c_str());
                if (!filter->is_valid()) {
                    valid = false;
                    break;
                }
                for (const auto &[name, value] : native.properties)
                    filter->set(name.c_str(), value.c_str());
                attached.filters.push_back(std::move(filter));
            }
            // mask_start creates the wrapped filter only when a frame
            // arrives, so check the service itself exists too.
            if (valid && attached.filters.size() > 1) {
                Mlt::Filter probe(profile, effect.service.c_str());
                valid = probe.is_valid();
            }
            if (!valid) {
                warnOnce(effect.service, "not available in this MLT; playing without it");
                ++extensionStats().skipped;
                continue;
            }
            for (const std::shared_ptr<Mlt::Filter> &filter : attached.filters) {
                if (cut)
                    engine::attachToCut(*cut, *filter);
                else
                    service.attach(*filter);
            }
            extensionStats().attached += static_cast<int>(attached.filters.size());
            m_attached[effect.id.value].push_back(std::move(attached));
        }
    }

    void warnOnce(const std::string &service, const char *why)
    {
        if (m_warned.insert(service).second)
            core::Log::warn("[effects] " + service + " " + why);
    }

    std::unordered_map<uint64_t, std::vector<Attached>> m_attached;
    std::set<std::string> m_warned;
};

} // namespace

std::unique_ptr<engine::EngineExtension> makeEffectsExtension()
{
    return std::make_unique<EffectsExtension>();
}

void setQuarantinedServices(std::set<std::string> services)
{
    std::lock_guard lock(quarantineMutex());
    quarantined() = std::move(services);
}

bool isQuarantined(const std::string &service)
{
    std::lock_guard lock(quarantineMutex());
    return quarantined().contains(service);
}

ExtensionStats &extensionStats()
{
    static ExtensionStats stats;
    return stats;
}

} // namespace ustudio::effects
