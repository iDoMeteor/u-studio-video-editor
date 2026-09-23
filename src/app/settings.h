#pragma once

#include <gio/gio.h>

#include <string>

namespace ustudio::app {

// Thin wrapper around this app's GSettings schema (com.ustudio.VideoEditor,
// data/com.ustudio.VideoEditor.gschema.xml). Every getter falls back to the
// same hardcoded default the corresponding call site used before this
// existed; every setter is a silent no-op when the schema wasn't found --
// see settings.cpp for why that degrade-don't-crash path is load-bearing,
// not defensive-programming boilerplate.
//
// Modularity note (the reason this is its own small class rather than four
// GSettings calls inlined at each call site): a future hotkey-rebinding
// feature needs the exact same schema-lookup/degrade plumbing to persist
// accelerator overrides. That should be a new key on the same schema and a
// new accessor pair here, not a second, separately-wired GSettings object.
class Settings
{
  public:
    Settings();
    ~Settings();

    Settings(const Settings &) = delete;
    Settings &operator=(const Settings &) = delete;

    static constexpr int kDefaultAutosaveDelayMinutes = 2;
    static constexpr const char *kDefaultPreviewScale = "auto";
    static constexpr double kDefaultShuttleMaxSpeed = 8.0;
    static constexpr int kDefaultRecentProjectsMax = 10;

    int autosaveDelayMinutes() const;
    void setAutosaveDelayMinutes(int minutes);

    // One of "auto"/"full"/"half"/"quarter" -- matches the transport bar's
    // preview-scale dropdown entries (buildUi()) and the gschema's
    // <choices>. Only consulted once, for that dropdown's initial
    // selection at startup; changing the dropdown afterwards does not
    // write back here (see showSettingsDialog()'s own comment).
    std::string defaultPreviewScale() const;
    void setDefaultPreviewScale(const std::string &scale);

    double shuttleMaxSpeed() const;
    void setShuttleMaxSpeed(double speed);

    int recentProjectsMax() const;
    void setRecentProjectsMax(int max);

    // False when the schema wasn't found (not installed, and
    // GSETTINGS_SCHEMA_DIR doesn't point at a compiled one) -- every
    // setter above is then a no-op. The Settings dialog shows a note when
    // this is false, so changes made there don't look silently accepted.
    bool isPersistent() const
    {
        return m_settings != nullptr;
    }

  private:
    GSettings *m_settings = nullptr;
};

} // namespace ustudio::app
