#pragma once

// Looks (doc 15, "Applying effects"): a saved stack of effects, applied to
// clips in one step. Brand Looks ship with the drop-in
// (data/looks/brand.json); the project's own live in Project::looks.

#include "core/json.h"
#include "core/model/types.h"

#include <string>
#include <vector>

namespace ustudio::effects {

// Looks from a looks file: {"version":1,"looks":[{"name","description",
// "effects":[{"service","params":{id:value},"mix"}]}]}. Numbers become
// scalars, booleans toggles, strings text; the effects are this drop-in's.
// A malformed entry is skipped.
std::vector<core::Look> looksFromJson(const Json &json);

// A look's effects, ready to add: fresh ids, the look's order.
std::vector<core::Effect> effectsOf(const core::Look &look);

} // namespace ustudio::effects
