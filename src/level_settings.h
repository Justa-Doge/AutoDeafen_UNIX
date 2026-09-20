#pragma once

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cmath>
#include <string>

#include "globals.h"

namespace level_settings {

inline constexpr float kPercentageComparisonEpsilon = 0.0001f;

inline int typeFor(GJGameLevel* level) {
    if (level->m_levelType != GJLevelType::Saved) return 1;
    if (level->m_dailyID > 0) return 2;
    if (level->m_gauntletLevel) return 3;
    return 0;
}

inline std::string keyFor(GJGameLevel* level) {
    auto levelId = level->m_levelID.value();
    const auto type = typeFor(level);

    if (type == 1) {
        levelId = level->m_M_ID;
    }

    return std::to_string(levelId) + "-" + std::to_string(type);
}

inline void load(GJGameLevel* level) {
    const auto defaultEnabled = Mod::get()->getSettingValue<bool>("default_enabled");
    const auto defaultPercentage = Mod::get()->getSettingValue<float>("default_percentage");

    state::currentLevelKey = keyFor(level);
    state::deafenEnabled = defaultEnabled;
    state::deafenPercentage = defaultPercentage;

    if (!Mod::get()->hasSavedValue(state::currentLevelKey)) return;

    const auto saved = Mod::get()->getSavedValue<matjson::Value>(state::currentLevelKey);
    if (saved["uses-default"].asBool().ok().value_or(false)) return;

    state::deafenEnabled = saved["enabled"].asBool().ok().value_or(
        saved["e"].asBool().ok().value_or(defaultEnabled)
    );
    state::deafenPercentage = std::clamp(
        saved["percentage"].as<float>().ok().value_or(
            saved["p"].as<float>().ok().value_or(defaultPercentage)
        ),
        0.f,
        100.f
    );
}

inline void save() {
    if (state::currentLevelKey.empty()) return;

    const auto defaultEnabled = Mod::get()->getSettingValue<bool>("default_enabled");
    const auto defaultPercentage = Mod::get()->getSettingValue<float>("default_percentage");
    const auto usesDefault =
        state::deafenEnabled == defaultEnabled &&
        std::fabs(state::deafenPercentage - defaultPercentage) < kPercentageComparisonEpsilon;

    auto saved = matjson::Value();
    saved["uses-default"] = usesDefault;

    if (!usesDefault) {
        saved["enabled"] = state::deafenEnabled;
        saved["percentage"] = state::deafenPercentage;
    }

    Mod::get()->setSavedValue(state::currentLevelKey, saved);
}

}
