#include "core/transitions.h"

#include "core/log.h"
#include "core/model/model.h"
#include "core/model/transition_native.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

namespace ustudio::effects {

std::vector<TransitionRecipe> recipesFromJson(const Json &json)
{
    std::vector<TransitionRecipe> recipes;
    if (json["version"].asNumber() != 1)
        return recipes;
    for (const Json &item : json["recipes"].asArray()) {
        if (!item["id"].isString() || !item["name"].isString())
            continue;
        TransitionRecipe recipe;
        recipe.id = item["id"].asString();
        recipe.name = item["name"].asString();
        recipe.category = item["category"].asString();
        recipe.description = item["description"].asString();
        for (const auto &[name, value] : item["params"].asObject()) {
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
            recipe.params.push_back(std::move(param));
        }
        for (const Json &name : item["exposed"].asArray()) {
            const std::string exposed = name.asString();
            if (std::any_of(recipe.params.begin(), recipe.params.end(),
                            [&](const core::Param &p) { return p.name == exposed; }))
                recipe.exposed.push_back(exposed);
        }
        core::Transition probe;
        probe.params = recipe.params;
        probe.length = 25;
        if (std::string problem = core::transitionProblem(probe); !problem.empty()) {
            core::Log::warn("[effects] transition recipe " + recipe.id + " skipped: " + problem);
            continue;
        }
        recipes.push_back(std::move(recipe));
    }
    return recipes;
}

std::vector<TransitionRecipe> loadRecipes(const std::string &folder)
{
    namespace fs = std::filesystem;
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(fs::path(folder), ec))
        if (entry.path().extension() == ".json")
            files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    std::vector<TransitionRecipe> recipes;
    std::set<std::string> ids;
    for (const fs::path &file : files) {
        std::ifstream in(file, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string error;
        std::optional<Json> json = parseJson(text, &error);
        if (!json) {
            core::Log::warn("[effects] transition recipes " + file.string() + " skipped: " + error);
            continue;
        }
        for (TransitionRecipe &recipe : recipesFromJson(*json))
            if (ids.insert(recipe.id).second)
                recipes.push_back(std::move(recipe));
    }
    // The plain dissolve first: the default, and what an unknown id plays.
    std::stable_partition(recipes.begin(), recipes.end(),
                          [](const TransitionRecipe &r) { return r.id == kDefaultRecipe; });
    return recipes;
}

int recipeIndexOf(const std::vector<TransitionRecipe> &recipes, const core::Transition &transition)
{
    if (recipes.empty())
        return -1;
    const std::string &id = transition.recipe.empty() ? std::string(kDefaultRecipe) : transition.recipe;
    for (size_t i = 0; i < recipes.size(); ++i)
        if (recipes[i].id == id)
            return static_cast<int>(i);
    return 0;
}

bool SetTransitionRecipe::apply(core::Model &model)
{
    if (!model.hasTransition(m_id))
        return false;
    const core::Transition &current = model.transition(m_id);
    if (model.track(current.track).locked)
        return false;
    core::Transition next = current;
    next.params = m_params;
    if (!core::transitionProblem(next).empty())
        return false;
    m_oldRecipe = current.recipe;
    m_oldParams = current.params;
    model.setTransitionRecipe(m_id, m_recipe, m_params);
    return true;
}

void SetTransitionRecipe::revert(core::Model &model)
{
    model.setTransitionRecipe(m_id, m_oldRecipe, m_oldParams);
}

bool SetTransitionRecipe::mergeWith(const core::Command &next)
{
    const auto *other = dynamic_cast<const SetTransitionRecipe *>(&next);
    if (!other || m_gesture == 0 || other->m_gesture != m_gesture || other->m_id != m_id)
        return false;
    m_recipe = other->m_recipe;
    m_params = other->m_params;
    return true;
}

bool SetTransitionRecipe::isNoOp() const
{
    return m_recipe == m_oldRecipe && m_params == m_oldParams;
}

std::vector<core::Param> withParam(std::vector<core::Param> params, const core::Param &param)
{
    for (core::Param &p : params)
        if (p.name == param.name) {
            p = param;
            return params;
        }
    params.push_back(param);
    return params;
}

} // namespace ustudio::effects
