#pragma once

#include "dropins/api.h"
#include "dropins/render_subcommand.h"
#include "engine/engine_extension.h"
#include "engine/factory_policy.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ustudio::app {
class ShellHost; // app/shell_host.h: GTK, so only its name here
} // namespace ustudio::app

namespace ustudio::dropins {

// IP5: what a drop-in does to the editor window once it exists (its
// inspector pages, actions, overlays; app/shell_host.h).
using ShellExtension = std::function<void(app::ShellHost &)>;

// IP4: search paths and MLT module directories a drop-in needs before
// Mlt::Factory::init() (engine/factory_policy.h).
using FactoryPaths = engine::FactoryPaths;

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

    // IP3: every graph this program builds (the live one, each render's)
    // gets its own instance from `factory` (engine/engine_extension.h).
    virtual void addEngineExtension(engine::EngineExtensionFactory factory) = 0;
    // IP6: a u-studio-render subcommand (render_subcommand.h). The editor's
    // host keeps them too but never runs them; a taken or invalid name is
    // refused with a warning.
    virtual void addRenderSubcommand(RenderSubcommand subcommand) = 0;
    // IP5: called once with the editor window's ShellHost after its UI is
    // built. The render tool's host keeps them too but has no window.
    virtual void addShellExtension(ShellExtension extension) = 0;
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
    void addEngineExtension(engine::EngineExtensionFactory factory) override;
    void addRenderSubcommand(RenderSubcommand subcommand) override;
    void addShellExtension(ShellExtension extension) override
    {
        m_shellExtensions.push_back(std::move(extension));
    }
    const std::vector<ShellExtension> &shellExtensions() const
    {
        return m_shellExtensions;
    }
    // In registration order.
    const std::vector<RenderSubcommand> &renderSubcommands() const
    {
        return m_renderSubcommands;
    }

  private:
    std::string m_program;
    std::vector<RenderSubcommand> m_renderSubcommands;
    std::vector<ShellExtension> m_shellExtensions;
};

} // namespace ustudio::dropins
