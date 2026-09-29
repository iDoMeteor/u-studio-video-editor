#pragma once

// A title clip's own field values (doc 16, "Fields"): the clip's
// `field.<name>` source params, which the engine hands to ustudio_title.
// One title file serves every guest; each clip says who.

#include "core/commands/command.h"
#include "core/model/model.h"

#include <cstdint>
#include <map>
#include <string>

namespace ustudio::titles {

// The clip's values, by field name. A field it doesn't set shows the
// title's default.
std::map<std::string, std::string> clipFieldValues(const core::Clip &clip);

// Sets a clip's field values to `values` exactly: a name left out goes back
// to the title's default. Other source params are kept. Commands with the
// same nonzero `gesture` (typing in one entry) merge into one undo step.
class SetClipFields : public core::Command
{
  public:
    SetClipFields(core::ClipId clip, std::map<std::string, std::string> values, uint64_t gesture = 0);
    std::string label() const override
    {
        return "Edit title fields";
    }
    bool apply(core::Model &model) override;
    void revert(core::Model &model) override;
    bool mergeWith(const core::Command &next) override;
    bool isNoOp() const override;

  private:
    core::ClipId m_clip;
    std::map<std::string, std::string> m_values;
    uint64_t m_gesture;
    std::vector<core::Param> m_old, m_new;
    // A caption clip's name follows its first line (captionName()) unless
    // the user renamed it.
    std::string m_oldName, m_newName;
};

} // namespace ustudio::titles
