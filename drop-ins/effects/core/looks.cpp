#include "core/looks.h"

#include "core/descriptor.h"

namespace ustudio::effects {

std::vector<core::Look> looksFromJson(const Json &json)
{
    std::vector<core::Look> looks;
    if (json["version"].asNumber() != 1)
        return looks;
    for (const Json &item : json["looks"].asArray()) {
        if (!item["name"].isString())
            continue;
        core::Look look;
        look.name = item["name"].asString();
        bool valid = true;
        for (const Json &e : item["effects"].asArray()) {
            if (!e["service"].isString()) {
                valid = false;
                break;
            }
            core::Effect effect;
            effect.service = e["service"].asString();
            effect.owner = kOwner;
            for (const auto &[name, value] : e["params"].asObject()) {
                core::Param param;
                param.name = name;
                if (value.isNumber())
                    param.value = value.asNumber();
                else if (value.isBool())
                    param.value = value.asBool();
                else if (value.isString())
                    param.value = value.asString();
                else
                    continue;
                effect.params.push_back(std::move(param));
            }
            if (e["mix"].isNumber())
                effect.mix.value = std::clamp(e["mix"].asNumber(), 0.0, 1.0);
            look.effects.push_back(std::move(effect));
        }
        if (valid && !look.effects.empty())
            looks.push_back(std::move(look));
    }
    return looks;
}

std::vector<core::Effect> effectsOf(const core::Look &look)
{
    std::vector<core::Effect> effects = look.effects;
    for (core::Effect &effect : effects)
        effect.id = {};
    return effects;
}

} // namespace ustudio::effects
