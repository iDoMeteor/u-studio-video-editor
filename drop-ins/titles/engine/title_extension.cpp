#include "title_extension.h"

#include "core/title_xml.h"

#include <variant>

namespace ustudio::titles {

namespace {

class TitleExtension : public engine::EngineExtension
{
  public:
    std::string name() const override
    {
        return "titles";
    }

    std::unique_ptr<Mlt::Producer> makeProducer(const core::Model &model, const core::Clip &clip,
                                                Mlt::Profile &profile) override
    {
        if (!model.hasAsset(clip.asset))
            return nullptr;
        const core::Asset &asset = model.asset(clip.asset);
        // A missing title plays as the missing-media placeholder, like any
        // other missing file.
        if (!isTitleFile(asset.path) || asset.status == core::Asset::Status::Missing)
            return nullptr;
        // The producer spans source frames 0..out: a clip cut from later in
        // the title (the right half of a split) still sees its outro at its
        // end.
        auto producer = makeTitleProducer(profile, asset.path, clip.out + 1, clip.sourceParams);
        // The clip's picture switched off: the engine's convention for
        // media (masterProducerFor()), which ustudio_title honours.
        if (producer && !clip.videoEnabled)
            producer->set("video_index", -1);
        return producer;
    }
};

} // namespace

std::unique_ptr<Mlt::Producer> makeTitleProducer(Mlt::Profile &profile, const std::string &path,
                                                 core::FrameIndex length, const std::vector<core::Param> &params)
{
    // Through MLT's loader ("service:resource"), not the factory directly:
    // the loader attaches the normalising filters that convert our RGBA to
    // what the graph asks for. Made directly, the frames' RGBA bytes were
    // read as YUV 4:2:2 by the compositor (a transparent frame came out
    // opaque green, 0,136,0: Y=U=V=0), MLT 7.40.
    const std::string spec = "ustudio_title:" + path;
    auto producer = std::make_unique<Mlt::Producer>(profile, "loader", spec.c_str());
    if (!producer->is_valid())
        return nullptr;
    producer->set("length", static_cast<int>(length));
    producer->set("out", static_cast<int>(length - 1));
    for (const core::Param &param : params)
        if (param.name.starts_with("field."))
            if (const auto *value = std::get_if<std::string>(&param.value))
                producer->set(param.name.c_str(), value->c_str());
    return producer;
}

std::unique_ptr<engine::EngineExtension> makeTitleExtension()
{
    return std::make_unique<TitleExtension>();
}

} // namespace ustudio::titles
