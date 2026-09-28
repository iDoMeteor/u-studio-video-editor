#include "engine/registry.h"

#include "core/log.h"
#include "engine/plugins.h"

#include <mlt++/Mlt.h>

#include <algorithm>
#include <fstream>
#include <iterator>

namespace ustudio::effects {

namespace fs = std::filesystem;

namespace {

std::optional<double> number(Mlt::Properties &properties, const char *name)
{
    if (!properties.get(name))
        return std::nullopt;
    return properties.get_double(name);
}

std::string text(Mlt::Properties &properties, const char *name)
{
    const char *value = properties.get(name);
    return value ? value : "";
}

bool yes(Mlt::Properties &properties, const char *name)
{
    const std::string value = text(properties, name);
    return value == "yes" || value == "1" || value == "true";
}

std::vector<std::string> list(Mlt::Properties &properties, const char *name)
{
    std::vector<std::string> out;
    Mlt::Properties items(static_cast<mlt_properties>(properties.get_data(name)));
    if (!items.is_valid())
        return out;
    for (int i = 0; i < items.count(); ++i)
        if (const char *item = items.get(i))
            out.emplace_back(item);
    return out;
}

// MLT's metadata YAML for one filter, into strings (core/descriptor.h).
RawEffect readMetadata(Mlt::Repository &repository, const std::string &service)
{
    RawEffect raw;
    raw.service = service;
    std::unique_ptr<Mlt::Properties> metadata(repository.metadata(mlt_service_filter_type, service.c_str()));
    if (!metadata || !metadata->is_valid())
        return raw;
    raw.title = text(*metadata, "title");
    raw.description = text(*metadata, "description");
    raw.tags = list(*metadata, "tags");
    Mlt::Properties params(static_cast<mlt_properties>(metadata->get_data("parameters")));
    if (!params.is_valid())
        return raw;
    for (int i = 0; i < params.count(); ++i) {
        Mlt::Properties p(static_cast<mlt_properties>(params.get_data(params.get_name(i))));
        if (!p.is_valid())
            continue;
        RawParam param;
        param.identifier = text(p, "identifier");
        param.title = text(p, "title");
        param.description = text(p, "description");
        param.type = text(p, "type");
        param.widget = text(p, "widget");
        param.minimum = number(p, "minimum");
        param.maximum = number(p, "maximum");
        param.defaultValue = text(p, "default");
        param.values = list(p, "values");
        param.animation = yes(p, "animation");
        param.readonly = yes(p, "readonly");
        raw.params.push_back(std::move(param));
    }
    return raw;
}

// frei0r's not_thread_safe.txt: plugin names (a version after '=' means
// "thread safe from"; MLT's check_thread_safe() compares, we only flag).
std::set<std::string> notThreadSafeFrei0r()
{
    std::set<std::string> names;
    const char *data = mlt_environment("MLT_DATA");
    if (!data)
        return names;
    std::ifstream in(fs::path(data) / "frei0r" / "not_thread_safe.txt");
    std::string line;
    while (std::getline(in, line)) {
        line = line.substr(0, line.find('#'));
        line = line.substr(0, line.find('='));
        while (!line.empty() && (line.back() == ' ' || line.back() == '\r'))
            line.pop_back();
        if (!line.empty())
            names.insert(line);
    }
    return names;
}

} // namespace

EffectRegistry::EffectRegistry(std::vector<EffectDescriptor> descriptors) : m_descriptors(std::move(descriptors))
{
    std::sort(m_descriptors.begin(), m_descriptors.end(),
              [](const EffectDescriptor &a, const EffectDescriptor &b) { return a.service < b.service; });
}

namespace {
EffectDescriptor describeWith(Mlt::Repository &repository, const std::string &service,
                              const std::vector<Json> &overlays, const std::set<std::string> &notThreadSafe)
{
    EffectDescriptor descriptor = normalise(readMetadata(repository, service));
    if (std::find(descriptor.tags.begin(), descriptor.tags.end(), "Hidden") != descriptor.tags.end() ||
        descriptor.name.find("DEPRECATED") != std::string::npos)
        descriptor.hidden = true;
    if (descriptor.family == "frei0r" && notThreadSafe.contains(service.substr(7)))
        descriptor.notThreadSafe = true;
    for (const Json &overlay : overlays)
        applyOverlay(descriptor, overlay[service]);
    return descriptor;
}
} // namespace

EffectDescriptor describe(Mlt::Repository &repository, const std::string &service, const std::vector<Json> &overlays)
{
    return describeWith(repository, service, overlays, notThreadSafeFrei0r());
}

EffectRegistry EffectRegistry::scan(Mlt::Repository &repository, const std::vector<Json> &overlays)
{
    const std::set<std::string> notThreadSafe = notThreadSafeFrei0r();
    std::vector<EffectDescriptor> descriptors;
    std::unique_ptr<Mlt::Properties> filters(repository.filters());
    for (int i = 0; filters && i < filters->count(); ++i) {
        const std::string service = filters->get_name(i);
        // The GPU engine's own (ADR-019: not user effects).
        if (familyOf(service) == "movit")
            continue;
        descriptors.push_back(describeWith(repository, service, overlays, notThreadSafe));
    }
    return EffectRegistry(std::move(descriptors));
}

const EffectDescriptor *EffectRegistry::find(const std::string &service) const
{
    auto it = std::lower_bound(m_descriptors.begin(), m_descriptors.end(), service,
                               [](const EffectDescriptor &d, const std::string &s) { return d.service < s; });
    return it != m_descriptors.end() && it->service == service ? &*it : nullptr;
}

Json EffectRegistry::toJson(const std::string &fingerprint) const
{
    Json out;
    out.set("schema", kRegistrySchema);
    out.set("fingerprint", fingerprint);
    Json::Array items;
    for (const EffectDescriptor &descriptor : m_descriptors)
        items.push_back(effects::toJson(descriptor));
    out.set("effects", Json(std::move(items)));
    return out;
}

std::optional<EffectRegistry> EffectRegistry::fromJson(const Json &json, const std::string &fingerprint)
{
    if (json["schema"].asNumber() != kRegistrySchema || json["fingerprint"].asString() != fingerprint)
        return std::nullopt;
    std::vector<EffectDescriptor> descriptors;
    for (const Json &item : json["effects"].asArray())
        if (std::optional<EffectDescriptor> descriptor = descriptorFromJson(item))
            descriptors.push_back(std::move(*descriptor));
    return EffectRegistry(std::move(descriptors));
}

std::vector<Json> loadOverlays(const fs::path &dir)
{
    std::vector<fs::path> files;
    std::error_code ec;
    for (const fs::directory_entry &entry : fs::directory_iterator(dir, ec))
        if (entry.path().extension() == ".json")
            files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    std::vector<Json> overlays;
    for (const fs::path &file : files) {
        std::ifstream in(file, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string error;
        std::optional<Json> json = parseJson(content, &error);
        if (!json || !json->isObject()) {
            core::Log::warn("[effects] overlay " + file.filename().string() +
                            " skipped: " + (json ? std::string("not an object") : error));
            continue;
        }
        overlays.push_back(std::move(*json));
    }
    return overlays;
}

fs::path effectsDataDir()
{
    std::error_code ec;
    if (fs::is_directory(EFFECTS_DATA_INSTALL_DIR, ec))
        return EFFECTS_DATA_INSTALL_DIR;
    return EFFECTS_DATA_SOURCE_DIR;
}

std::string registryFingerprint()
{
    std::string salt = std::string("mlt ") + mlt_version_get_string() + " effects " + USTUDIO_VERSION + " schema " +
                       std::to_string(kRegistrySchema);
    for (const Json &overlay : loadOverlays(effectsDataDir() / "overlays"))
        salt += "\n" + effects::toJson(overlay);
    return pluginSetFingerprint(findFrei0rPlugins(frei0rSearchDirs()), salt);
}

} // namespace ustudio::effects
