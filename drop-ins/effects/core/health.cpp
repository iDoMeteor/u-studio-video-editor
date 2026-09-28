#include "core/health.h"

#include <fstream>
#include <iterator>

namespace ustudio::effects {

namespace {

constexpr HealthStatus kStatuses[] = {HealthStatus::Ok, HealthStatus::Crashed, HealthStatus::TimedOut,
                                      HealthStatus::BadOutput, HealthStatus::Unavailable};

Json recordToJson(const HealthRecord &record)
{
    Json out;
    out.set("status", healthStatusName(record.status));
    out.set("reason", record.reason);
    out.set("ms_per_frame", record.msPerFrame);
    return out;
}

std::optional<HealthRecord> recordFromJson(const Json &json)
{
    std::optional<HealthStatus> status = healthStatusFromName(json["status"].asString());
    if (!status)
        return std::nullopt;
    return HealthRecord{*status, json["reason"].asString(), json["ms_per_frame"].asNumber()};
}

} // namespace

const char *healthStatusName(HealthStatus status)
{
    switch (status) {
    case HealthStatus::Ok:
        return "ok";
    case HealthStatus::Crashed:
        return "crashed";
    case HealthStatus::TimedOut:
        return "timed_out";
    case HealthStatus::BadOutput:
        return "bad_output";
    case HealthStatus::Unavailable:
        return "unavailable";
    }
    return "crashed";
}

std::optional<HealthStatus> healthStatusFromName(const std::string &name)
{
    for (HealthStatus status : kStatuses)
        if (name == healthStatusName(status))
            return status;
    return std::nullopt;
}

CostBadge costBadge(double msPerFrame)
{
    if (msPerFrame < 8.0)
        return CostBadge::Light;
    if (msPerFrame < 25.0)
        return CostBadge::Medium;
    return CostBadge::Heavy;
}

std::string probeResultLine(const std::string &service, const HealthRecord &record)
{
    Json line = recordToJson(record);
    line.set("service", service);
    return toJson(line);
}

std::optional<std::pair<std::string, HealthRecord>> parseProbeResultLine(const std::string &line)
{
    std::optional<Json> json = parseJson(line);
    if (!json || !(*json)["service"].isString())
        return std::nullopt;
    std::optional<HealthRecord> record = recordFromJson(*json);
    if (!record)
        return std::nullopt;
    return std::make_pair((*json)["service"].asString(), *record);
}

std::string probeStageLine(const std::string &service, const std::string &stage)
{
    Json line;
    line.set("service", service);
    line.set("stage", stage);
    return toJson(line);
}

std::optional<std::string> parseProbeStageLine(const std::string &line)
{
    std::optional<Json> json = parseJson(line);
    if (!json || !(*json)["stage"].isString())
        return std::nullopt;
    return (*json)["stage"].asString();
}

HealthRecord interpretProbe(const std::string &service, const std::string &output, bool timedOut)
{
    std::optional<std::string> lastStage;
    size_t start = 0;
    while (start <= output.size()) {
        size_t end = output.find('\n', start);
        if (end == std::string::npos)
            end = output.size();
        const std::string line = output.substr(start, end - start);
        if (auto result = parseProbeResultLine(line); result && result->first == service)
            return result->second;
        if (auto stage = parseProbeStageLine(line))
            lastStage = stage;
        start = end + 1;
    }
    const std::string where = lastStage ? " (" + *lastStage + ")" : "";
    if (timedOut)
        return {HealthStatus::TimedOut, "still running at the deadline" + where, 0.0};
    return {HealthStatus::Crashed, "the probe died" + where, 0.0};
}

std::optional<HealthRecord> HealthFile::find(const std::string &service) const
{
    auto it = records.find(service);
    if (it == records.end())
        return std::nullopt;
    return it->second;
}

bool HealthFile::quarantined(const std::string &service) const
{
    std::optional<HealthRecord> record = find(service);
    return record && !record->usable();
}

Json toJson(const HealthFile &file)
{
    Json out;
    out.set("version", 1);
    out.set("fingerprint", file.fingerprint);
    Json records;
    for (const auto &[service, record] : file.records)
        records.set(service, recordToJson(record));
    out.set("records", records.isNull() ? Json(Json::Object{}) : std::move(records));
    return out;
}

HealthFile healthFileFromJson(const Json &json)
{
    HealthFile file;
    if (json["version"].asNumber() != 1)
        return file;
    file.fingerprint = json["fingerprint"].asString();
    for (const auto &[service, item] : json["records"].asObject())
        if (std::optional<HealthRecord> record = recordFromJson(item))
            file.records.emplace(service, *record);
    return file;
}

HealthFile loadHealthFile(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::optional<Json> json = parseJson(text);
    return json ? healthFileFromJson(*json) : HealthFile{};
}

bool saveHealthFile(const std::filesystem::path &path, const HealthFile &file)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::filesystem::path temp = path.string() + ".part";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << toJson(toJson(file)) << '\n';
        if (!out)
            return false;
    }
    std::filesystem::rename(temp, path, ec);
    return !ec;
}

} // namespace ustudio::effects
