#pragma once

// The effects drop-in's EngineExtension (IP3; doc 15, "Engine mapping"):
// every effect it owns (core::Effect::owner == "effects") becomes the MLT
// filters core::nativeFilters() names, on every cut that plays its clip
// (dissolve tails and heads included, keyframes shifted per cut), on its
// track's playlist, or on the whole tractor (master effects). A parameter or
// mix change that keeps the filters' shape is set on the live filters
// (applyInPlace()); anything else rebuilds.
//
// With the GPU pipeline on (ADR-019) these are CPU filters inside the movit
// graph: MLT downloads the frame before the first and uploads it after the
// last of a run on one cut, so a run costs one transfer, not one per effect.

#include "engine/engine_extension.h"

#include <atomic>
#include <memory>
#include <set>
#include <string>

namespace ustudio::effects {

std::unique_ptr<engine::EngineExtension> makeEffectsExtension();

// Services the health scan quarantined (core/health.h): never attached, so
// a project naming one plays without it rather than crashing. Replaced
// whole by the scan; read by every graph build (any thread).
void setQuarantinedServices(std::set<std::string> services);
bool isQuarantined(const std::string &service);

// Counts for tests and the log: filters attached, and changes applied in
// place, since the last reset (every graph's builds add to them).
struct ExtensionStats
{
    std::atomic<int> attached{0};
    std::atomic<int> skipped{0}; // quarantined, unknown to MLT, or another drop-in's
    std::atomic<int> inPlace{0};

    void reset()
    {
        attached = 0;
        skipped = 0;
        inPlace = 0;
    }
};
ExtensionStats &extensionStats();

} // namespace ustudio::effects
