#pragma once

#include "dropins/api.h"

#include <string>
#include <utility>
#include <vector>

namespace ustudio::dropins {

// IP4: search paths and MLT module directories a drop-in needs before
// Mlt::Factory::init() (the curated FREI0R_PATH, titles' MLT module).
struct FactoryPaths
{
    std::vector<std::string> frei0rPaths;
    std::vector<std::string> ofxPaths;
    std::vector<std::string> mltModuleDirs;
};

// What registerDropIn() receives: the integration points IP3, IP5 and IP6
// (doc 15). Pure virtual, so a module calls it through the vtable and links
// nothing from the app. Each integration point adds its methods here when
// it lands (and bumps DROPIN_API_VERSION).
class DropInHost
{
  public:
    virtual ~DropInHost() = default;

    // Which program is hosting: "editor" or "render" (the render tool has no
    // UI hosts).
    virtual std::string program() const = 0;
    // Through the app's log (a drop-in names itself: "[effects] ...").
    virtual void log(const std::string &message) = 0;
};

// The host with no integration points of its own yet: logging and which
// program it is. The editor's and the render tool's hosts grow from it as
// the integration points land.
class BasicDropInHost : public DropInHost
{
  public:
    explicit BasicDropInHost(std::string program) : m_program(std::move(program)) {}
    std::string program() const override
    {
        return m_program;
    }
    void log(const std::string &message) override;

  private:
    std::string m_program;
};

} // namespace ustudio::dropins
