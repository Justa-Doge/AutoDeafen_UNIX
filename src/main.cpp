#include <Geode/Geode.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include "globals.h"
#include "gui.h"
#include "helpers.h"
#include "ipc.h"
#include "level_settings.h"
#include "linux_setup.h"

using namespace geode::prelude;

namespace {

void resetAttemptState() {
    state::deafenedThisAttempt = false;
    state::playerDiedThisAttempt = false;
}

void endAttempt() {
    if (state::deafenedThisAttempt && !state::playerDiedThisAttempt) {
        ipc::setDeafened(false);
    }

    resetAttemptState();
}

void loadAuthentication() {
    auto mod = Mod::get();

    if (mod->hasSavedValue("CLIENT_ID")) {
        state::clientId = helpers::trimAsciiWhitespace(
            mod->getSavedValue<std::string>("CLIENT_ID")
        );
    }
    if (mod->hasSavedValue("CLIENT_SECRET")) {
        state::clientSecret = helpers::trimAsciiWhitespace(
            mod->getSavedValue<std::string>("CLIENT_SECRET")
        );
    }
    if (!mod->hasSavedValue("DISCORD_ACCESS_TOKEN") ||
        !mod->hasSavedValue("DISCORD_REFRESH_TOKEN") ||
        !mod->hasSavedValue("TOKEN_EXPIRY")) {
        return;
    }

    state::refreshToken = mod->getSavedValue<std::string>("DISCORD_REFRESH_TOKEN");
    state::tokenExpiry = mod->getSavedValue<long long>("TOKEN_EXPIRY");

    if (helpers::currentTime() < state::tokenExpiry) {
        state::accessToken = mod->getSavedValue<std::string>("DISCORD_ACCESS_TOKEN");
        helpers::initializeDiscordIpc();
    }
    helpers::refreshDiscordAuthIfNeeded();
}

}

$on_mod(Loaded) {
    linux_setup::registerSetting();
    loadAuthentication();
};

class $modify(PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        resetAttemptState();
        level_settings::load(level);
        return true;
    }

    void postUpdate(float deltaTime) {
        PlayLayer::postUpdate(deltaTime);
        helpers::refreshDiscordAuthIfNeeded();

        const auto shouldManageVoice = state::deafenEnabled &&
            (!m_isPracticeMode || Mod::get()->getSettingValue<bool>("practice"));

        if (!shouldManageVoice || m_hasCompletedLevel) {
            endAttempt();
            return;
        }

        if (getCurrentPercent() >= state::deafenPercentage && !state::deafenedThisAttempt) {
            ipc::setDeafened(true);
            state::deafenedThisAttempt = true;
        }
    }

    void resetLevel() {
        if (state::deafenedThisAttempt && !state::playerDiedThisAttempt) {
            ipc::setDeafened(false);
        }

        PlayLayer::resetLevel();
        resetAttemptState();
    }

    void onExit() {
        endAttempt();
        PlayLayer::onExit();
    }
};

class $modify(PlayerObject) {
    void playerDestroyed(bool destroyedByPlayer) {
        const auto playLayer = PlayLayer::get();
        const auto isNormalAttempt = playLayer && playLayer->m_level &&
            this == playLayer->m_player1 &&
            !playLayer->m_level->isPlatformer();

        if (isNormalAttempt && state::deafenedThisAttempt && !state::playerDiedThisAttempt) {
            ipc::setDeafened(false);
            state::playerDiedThisAttempt = true;
        }

        PlayerObject::playerDestroyed(destroyedByPlayer);
    }
};

class $modify(AutoDeafenPauseLayer, PauseLayer) {
    void onQuit(CCObject* sender) {
        level_settings::save();
        PauseLayer::onQuit(sender);
    }

    void onAutoDeafenMenuClick(CCObject*) {
        if (state::accessToken.empty()) {
            gui::openSetupPopup();
            return;
        }

        gui::openModPopup();
    }

    void customSetup() {
        endAttempt();
        PauseLayer::customSetup();

        const auto menu = getChildByID("right-button-menu");
        const auto sprite = CCSprite::createWithSpriteFrameName("GJ_musicOffBtn_001.png");
        if (!menu || !sprite) return;

        const auto button = CCMenuItemSpriteExtra::create(
            sprite,
            sprite,
            this,
            menu_selector(AutoDeafenPauseLayer::onAutoDeafenMenuClick)
        );
        menu->addChild(button);
        menu->updateLayout();
    }
};
