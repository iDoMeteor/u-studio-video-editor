#pragma once

// EffectRegistry (doc 15, "Architecture"): every filter MLT registered in
// this process, as EffectDescriptors with the curated overlays applied.
// Built from Mlt::Repository metadata, which isn't safe to read beside a
// running graph, so the editor never builds it in its own process: the
// render tool's --effects-registry prints it (engine/probe.cpp) and the
// editor caches that (app/health_scan.cpp). Tests and the render tool build
// it directly.

#include "core/descriptor.h"
#include "core/json.h"

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Mlt {
class Repository;
}

namespace ustudio::effects {

// Bumped whenever normalisation or the cache's shape changes, so an old
// cache is rebuilt rather than misread.
inline constexpr int kRegistrySchema = 1;

class EffectRegistry
{
  public:
    EffectRegistry() = default;
    explicit EffectRegistry(std::vector<EffectDescriptor> descriptors);

    // Every filter in `repository`, normalised, then `overlays` (each file's
    // object keyed by service id) in order. Families this drop-in never
    // offers (the GPU engine's movit.*) are left out; plumbing is kept but
    // hidden, so a project naming it still has a descriptor.
    static EffectRegistry scan(Mlt::Repository &repository, const std::vector<Json> &overlays);

    const std::vector<EffectDescriptor> &descriptors() const
    {
        return m_descriptors;
    }
    const EffectDescriptor *find(const std::string &service) const;

    Json toJson(const std::string &fingerprint) const;
    // Nullopt when `json` is from another schema or another fingerprint.
    static std::optional<EffectRegistry> fromJson(const Json &json, const std::string &fingerprint);

  private:
    std::vector<EffectDescriptor> m_descriptors; // sorted by service
};

// One filter's descriptor, as scan() would make it.
EffectDescriptor describe(Mlt::Repository &repository, const std::string &service, const std::vector<Json> &overlays);

// The overlay files in `dir` (*.json, by name), parsed; a malformed one is
// logged and skipped.
std::vector<Json> loadOverlays(const std::filesystem::path &dir);

// The drop-in's data directory: installed, or this source tree's when
// running from a build directory.
std::filesystem::path effectsDataDir();

// The registry's fingerprint: the plugin set (engine/plugins.h), the MLT
// version, the drop-in's version and schema, and the overlays' contents.
std::string registryFingerprint();

} // namespace ustudio::effects
