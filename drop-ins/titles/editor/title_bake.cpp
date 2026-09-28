#include "title_bake.h"

#include "core/commands/composite_command.h"
#include "core/commands/primitives.h"
#include "core/media/utf8_path.h"
#include "core/title_xml.h"
#include "engine/engine_sync.h"
#include "engine/title_export.h"
#include "jobs.h"

#include <filesystem>
#include <memory>
#include <set>

namespace ustudio::titles {

namespace {

// Bakes in flight, so two at once never pick the same name.
std::set<std::string> &reserved()
{
    static std::set<std::string> paths;
    return paths;
}

core::Asset bakedAsset(const std::string &path, const engine::EngineSync::ProbedMedia &probed,
                       const core::Rational &sequenceFps)
{
    core::Asset asset;
    asset.path = path;
    asset.displayName = core::utf8String(core::pathFromUtf8(path).filename());
    asset.fileFingerprint = probed.fingerprint;
    asset.info.hasVideo = true;
    asset.info.lengthInSequenceFrames = probed.length;
    asset.info.fps = probed.fps;
    asset.info.width = probed.width;
    asset.info.height = probed.height;
    asset.info.sar = {1, 1};
    asset.info.container = "mov";
    asset.info.nativeDurationSeconds = static_cast<double>(probed.length) * static_cast<double>(sequenceFps.den) /
                                       static_cast<double>(sequenceFps.num);
    asset.status = core::Asset::Status::Ready;
    return asset;
}

} // namespace

std::string bakePath(const std::string &titlePath)
{
    namespace fs = std::filesystem;
    const fs::path title = core::pathFromUtf8(titlePath);
    const std::string stem = core::utf8String(title.stem());
    std::error_code ec;
    for (int n = 1;; ++n) {
        const std::string name = stem + (n == 1 ? " (baked).mov" : " (baked " + std::to_string(n) + ").mov");
        const std::string path = core::utf8String(title.parent_path() / core::pathFromUtf8(name));
        if (!fs::exists(core::pathFromUtf8(path), ec) && !reserved().contains(path))
            return path;
    }
}

void bakeTitleClip(app::ShellHost &host, core::ClipId id)
{
    const core::Model &model = host.model();
    if (!model.hasClip(id))
        return;
    const core::Clip &clip = model.clip(id);
    if (!model.hasAsset(clip.asset) || !isTitleFile(model.asset(clip.asset).path)) {
        host.showStatus("Select a title clip to bake it.");
        return;
    }
    const core::Asset &title = model.asset(clip.asset);
    const core::Profile profile = model.sequence().profile;
    TitleExportRequest request;
    request.title = title.path;
    request.output = bakePath(title.path);
    request.format = "prores";
    // Source frames 0..out, as the clip's producer spans them, so a clip cut
    // from later in the title keeps its place in the animation.
    request.frames = clip.out + 1;
    request.fps = profile.fps;
    request.timelineStart = static_cast<double>(clip.position - clip.in);
    for (const core::Param &param : clip.sourceParams)
        if (param.name.starts_with("field."))
            request.fields.push_back(param);
    const core::AssetId titleAsset = clip.asset;
    const std::vector<core::Param> params = clip.sourceParams;
    const std::string name = title.displayName;
    reserved().insert(request.output);
    host.showStatus("Baking " + name + "…");

    app::ShellHost *hostPtr = &host; // the window lives for the process
    startJob([request, profile, id, titleAsset, params, name, hostPtr] {
        auto written = exportTitle(request, &jobsCancelled());
        engine::EngineSync::ProbedMedia probed;
        if (written)
            probed = engine::EngineSync::probeMediaFile(profile, request.output);
        postToMain([request, written, probed, profile, id, titleAsset, params, name, hostPtr] {
            reserved().erase(request.output);
            app::ShellHost &shell = *hostPtr;
            if (!written) {
                if (written.error() != "cancelled")
                    shell.showStatus("Couldn't bake " + name + ": " + written.error());
                return;
            }
            const std::string file = core::utf8String(core::pathFromUtf8(request.output).filename());
            if (probed.length <= 0) {
                shell.showStatus("Baked " + name + " to " + file + ", but it doesn't open.");
                return;
            }
            // Swapped in only if the clip still plays the title it was
            // baked from, with the same fields; the file stays either way.
            const core::Model &now = shell.model();
            if (!now.hasClip(id) || now.clip(id).asset != titleAsset || now.clip(id).sourceParams != params) {
                shell.showStatus("Baked " + name + " to " + file +
                                 "; the clip changed meanwhile, so it's in the bin "
                                 "only.");
                std::vector<std::unique_ptr<core::Command>> steps;
                steps.push_back(std::make_unique<core::AddAsset>(bakedAsset(request.output, probed, profile.fps)));
                shell.execute(std::make_unique<core::CompositeCommand>("Add " + file, std::move(steps)));
                return;
            }
            std::vector<std::unique_ptr<core::Command>> steps;
            const core::AssetId baked{now.project().nextId}; // AddAsset takes the next id
            steps.push_back(std::make_unique<core::AddAsset>(bakedAsset(request.output, probed, profile.fps)));
            steps.push_back(std::make_unique<core::SetClipAsset>(id, baked, std::vector<core::Param>{}, "Bake title"));
            if (shell.execute(std::make_unique<core::CompositeCommand>("Bake " + name, std::move(steps))))
                shell.showStatus("Baked " + name + " to " + file + ". Undo brings the live title back.");
            else
                shell.showStatus("Baked " + name + " to " + file + ", but couldn't swap it in (is the track locked?).");
        });
    });
}

void exportTitleClip(app::ShellHost &host, core::ClipId id, const std::string &format, const std::string &output)
{
    const core::Model &model = host.model();
    if (!model.hasClip(id))
        return;
    const core::Clip &clip = model.clip(id);
    if (!model.hasAsset(clip.asset) || !isTitleFile(model.asset(clip.asset).path))
        return;
    TitleExportRequest request;
    request.title = model.asset(clip.asset).path;
    request.output = output;
    request.format = format;
    request.frames = clip.length();
    request.fps = model.sequence().profile.fps;
    request.timelineStart = static_cast<double>(clip.position);
    for (const core::Param &param : clip.sourceParams)
        if (param.name.starts_with("field."))
            request.fields.push_back(param);
    const std::string file = core::utf8String(core::pathFromUtf8(output).filename());
    host.showStatus("Exporting " + file + "…");
    app::ShellHost *hostPtr = &host; // the window lives for the process
    startJob([request, file, hostPtr] {
        auto written = exportTitle(request, &jobsCancelled());
        postToMain([written, file, hostPtr] {
            if (written)
                hostPtr->showStatus("Exported " + file + ".");
            else if (written.error() != "cancelled")
                hostPtr->showStatus("Couldn't export " + file + ": " + written.error());
        });
    });
}

} // namespace ustudio::titles
