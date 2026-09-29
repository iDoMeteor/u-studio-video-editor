#include "test_extension.h"

#include "core/model/animation.h"

#include <unordered_map>

namespace ustudio::testdropin {

ExtensionLog &extensionLog()
{
    static ExtensionLog log;
    return log;
}

bool &useTestCompositor()
{
    static bool on = false;
    return on;
}

namespace {

class TestExtension : public engine::EngineExtension
{
  public:
    explicit TestExtension(std::string owner) : m_owner(std::move(owner)) {}
    std::string name() const override
    {
        return m_owner;
    }

    void beginBuild() override
    {
        m_filters.clear();
        ++extensionLog().builds;
    }

    void decorateCut(Mlt::Producer &cut, const engine::CutContext &context) override
    {
        extensionLog().cuts.emplace_back(context.offset, context.length);
        attachEffects(cut, context.clip.effects, context.offset, context.length, context.profile, &cut);
    }

    void decoratePlaylist(Mlt::Playlist &playlist, const core::Model &model, const core::Track &track,
                          Mlt::Profile &profile) override
    {
        ++extensionLog().playlists;
        attachEffects(playlist, track.effects, 0, std::max<core::FrameIndex>(model.sequence().length(), 1), profile);
    }

    void decorateLane(Mlt::Service &, const core::Model &, int lane,
                      const std::vector<const core::AdjustmentBlock *> &blocks, Mlt::Profile &) override
    {
        std::vector<core::FrameIndex> starts;
        for (const core::AdjustmentBlock *block : blocks)
            starts.push_back(block->start);
        extensionLog().lanes.emplace_back(lane, std::move(starts));
    }

    void decorateTractor(Mlt::Tractor &tractor, const core::Model &model, Mlt::Profile &profile) override
    {
        ++extensionLog().tractors;
        attachEffects(tractor, model.sequence().effects, 0, std::max<core::FrameIndex>(model.sequence().length(), 1),
                      profile);
    }

    std::unique_ptr<Mlt::Producer> makeProducer(const core::Model &, const core::Clip &clip,
                                                Mlt::Profile &profile) override
    {
        for (const core::Param &param : clip.sourceParams) {
            if (param.name != "color" || !std::holds_alternative<std::string>(param.value))
                continue;
            ++extensionLog().producers;
            auto producer =
                std::make_unique<Mlt::Producer>(profile, "color", std::get<std::string>(param.value).c_str());
            producer->set("length", 100'000);
            producer->set_in_and_out(0, 99'999);
            return producer;
        }
        return nullptr;
    }

    std::unique_ptr<Mlt::Tractor> makeTransitionSegment(const core::Model &, const core::Transition &transition,
                                                        Mlt::Producer &, Mlt::Producer &headB,
                                                        Mlt::Profile &profile) override
    {
        if (transition.recipe != "hard-cut")
            return nullptr;
        ++extensionLog().recipes;
        auto segment = std::make_unique<Mlt::Tractor>(profile);
        segment->set_track(headB, 0);
        segment->set("ustudio.test_recipe", 1);
        return segment;
    }

    std::unique_ptr<Mlt::Transition> compositor(Mlt::Profile &profile, int, int) override
    {
        if (!useTestCompositor())
            return nullptr;
        ++extensionLog().compositors;
        auto transition = std::make_unique<Mlt::Transition>(profile, "composite");
        transition->set("ustudio.test_compositor", 1);
        return transition;
    }

    bool applyInPlace(const engine::ParamChange &change) override
    {
        auto found = m_filters.find(change.effect.id.value);
        if (found == m_filters.end())
            return false;
        for (const std::string &name : change.params)
            if (name != "level" && name != "mix")
                return false; // anything else rebuilds
        for (Attached &attached : found->second)
            setParams(*attached.filter, change.effect, attached.offset, attached.length);
        ++extensionLog().inPlace;
        return true;
    }

  private:
    struct Attached
    {
        std::shared_ptr<Mlt::Filter> filter;
        core::FrameIndex offset, length;
    };

    static void setParams(Mlt::Filter &filter, const core::Effect &effect, core::FrameIndex offset,
                          core::FrameIndex length)
    {
        for (const core::Param &param : effect.params) {
            if (!std::holds_alternative<double>(param.value))
                continue;
            // As a string, even a constant: setting a number over an
            // animated property leaves MLT's parsed animation in place.
            if (param.keyframes.empty())
                filter.set(param.name.c_str(), core::formatDouble(std::get<double>(param.value)).c_str());
            else
                filter.set(param.name.c_str(),
                           core::animationString(core::keyframesForCut(param.keyframes, offset, length)).c_str());
        }
    }

    // `cut` when `service` is a clip's cut (engine::attachToCut()).
    void attachEffects(Mlt::Service &service, const std::vector<core::Effect> &effects, core::FrameIndex offset,
                       core::FrameIndex length, Mlt::Profile &profile, Mlt::Producer *cut = nullptr)
    {
        for (const core::Effect &effect : effects) {
            if (effect.owner != m_owner || !effect.enabled)
                continue;
            auto filter = std::make_shared<Mlt::Filter>(profile, effect.service.c_str());
            setParams(*filter, effect, offset, length);
            if (cut)
                engine::attachToCut(*cut, *filter);
            else
                service.attach(*filter);
            m_filters[effect.id.value].push_back({filter, offset, length});
        }
    }

    std::string m_owner;
    std::unordered_map<uint64_t, std::vector<Attached>> m_filters;
};

} // namespace

std::unique_ptr<engine::EngineExtension> makeTestExtension(std::string owner)
{
    return std::make_unique<TestExtension>(std::move(owner));
}

} // namespace ustudio::testdropin
