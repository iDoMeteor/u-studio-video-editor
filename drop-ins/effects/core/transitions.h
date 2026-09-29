#pragma once

// Transition recipes (doc 15, "Transitions", FX3): dissolves, dips, flashes
// and wipes, shipped as data (data/transitions/*.json). A recipe is a name
// and the Transition::params it resolves into (core/model/
// transition_native.h says what they mean); choosing one is a
// SetTransitionRecipe, one undo step, saved with the project.

#include "core/json.h"
#include "core/commands/command.h"
#include "core/model/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ustudio::effects {

struct TransitionRecipe
{
    std::string id;       // "wipe.radial": what Transition::recipe records
    std::string name;     // "Radial Wipe"
    std::string category; // "Dissolves", "Wipes"
    std::string description;
    std::string swatch; // "#rrggbb": a dip's or flash's middle colour, for its tile; "" otherwise
    std::vector<core::Param> params;  // empty: the plain dissolve
    std::vector<std::string> exposed; // params the user may change (softness, reverse)
};

// The id every transition has before a recipe is chosen.
inline constexpr const char *kDefaultRecipe = "dissolve";

// Recipes from a recipes file: {"version":1,"recipes":[{"id","name",
// "category","description","swatch","params":{name:value},"exposed":[name]}]}.
// Numbers become doubles, booleans bools, strings strings. An entry without
// an id or name, or whose params core::transitionProblem() refuses, is
// skipped; so is an exposed name that isn't among its params.
std::vector<TransitionRecipe> recipesFromJson(const Json &json);

// Every recipe file in `folder` (*.json, by file name), ids unique (the
// first wins). A file that doesn't parse is skipped with a warning.
std::vector<TransitionRecipe> loadRecipes(const std::string &folder);

// The recipe a transition plays: the one with its recipe id, else the plain
// dissolve (index 0 when present). -1 when `recipes` is empty.
int recipeIndexOf(const std::vector<TransitionRecipe> &recipes, const core::Transition &transition);

// Chooses a transition's recipe and params (a recipe from the list, or the
// same recipe with one exposed param changed). Refused on a locked track or
// when core::transitionProblem() refuses the params. Changes of one gesture
// (a softness slider drag) merge.
class SetTransitionRecipe : public core::Command
{
  public:
    SetTransitionRecipe(core::TransitionId id, std::string recipe, std::vector<core::Param> params,
                        uint64_t gesture = 0)
        : m_id(id), m_recipe(std::move(recipe)), m_params(std::move(params)), m_gesture(gesture)
    {}
    std::string label() const override
    {
        return "Change transition";
    }
    bool apply(core::Model &model) override;
    void revert(core::Model &model) override;
    bool mergeWith(const core::Command &next) override;
    bool isNoOp() const override;

  private:
    core::TransitionId m_id;
    std::string m_recipe, m_oldRecipe;
    std::vector<core::Param> m_params, m_oldParams;
    uint64_t m_gesture;
};

// `params` with `param` replacing the one of its name (or added).
std::vector<core::Param> withParam(std::vector<core::Param> params, const core::Param &param);

} // namespace ustudio::effects
