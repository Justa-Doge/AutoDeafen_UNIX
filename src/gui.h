#pragma once

#include <Geode/Geode.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/utils/web.hpp>

#include <algorithm>
#include <cmath>
#include <string>

using namespace geode::prelude;

#include "globals.h"
#include "helpers.h"
#include "level_settings.h"
#include "oauth.h"

namespace gui {

inline void openUrl(const std::string& url) {
    geode::utils::web::openLinkInBrowser(url);
}

inline std::string formatPercentage(float value) {
    auto formatted = std::to_string(std::round(value * 100000.f) / 100000.f);
    formatted.erase(formatted.find_last_not_of('0') + 1);

    if (!formatted.empty() && formatted.back() == '.') {
        formatted.pop_back();
    }

    return formatted;
}

inline void openAuthSetup();
inline void openSetupPopup();
inline void openModPopup();

}

class AuthLayer final : public geode::Popup {
public:
    static AuthLayer* create() {
        auto layer = new AuthLayer;
        if (layer && layer->init()) {
            layer->autorelease();
            return layer;
        }

        delete layer;
        return nullptr;
    }

protected:
    bool init() override {
        if (!Popup::init(300.f, 240.f)) return false;

        const auto topMiddle = ccp(m_size.width / 2.f, m_size.height);

        auto title = CCLabelBMFont::create("Discord Setup", "goldFont.fnt");
        title->setPosition(topMiddle + ccp(0.f, 5.f));

        m_clientIdStatus = CCLabelBMFont::create("", "chatFont-uhd.fnt");
        m_clientIdStatus->setScale(0.5f);
        m_clientIdStatus->setPosition(topMiddle + ccp(0.f, -60.f));

        m_clientSecretStatus = CCLabelBMFont::create("", "chatFont-uhd.fnt");
        m_clientSecretStatus->setScale(0.5f);
        m_clientSecretStatus->setPosition(topMiddle + ccp(0.f, -120.f));

        auto tutorialButton = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Open Guide"),
            this,
            menu_selector(AuthLayer::openTutorial)
        );
        tutorialButton->setPosition(topMiddle + ccp(0.f, -30.f));

        auto clientIdButton = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Paste Client ID"),
            this,
            menu_selector(AuthLayer::pasteClientId)
        );
        clientIdButton->setPosition(topMiddle + ccp(0.f, -90.f));

        auto clientSecretButton = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Paste Client Secret"),
            this,
            menu_selector(AuthLayer::pasteClientSecret)
        );
        clientSecretButton->setPosition(topMiddle + ccp(0.f, -150.f));

        auto continueButton = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Continue"),
            this,
            menu_selector(AuthLayer::finishSetup)
        );
        continueButton->setPosition(topMiddle + ccp(0.f, -200.f));

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        menu->addChild(tutorialButton);
        menu->addChild(clientIdButton);
        menu->addChild(clientSecretButton);
        menu->addChild(continueButton);

        m_mainLayer->addChild(title);
        m_mainLayer->addChild(m_clientIdStatus);
        m_mainLayer->addChild(m_clientSecretStatus);
        m_mainLayer->addChild(menu);

        updateCredentialStatus();
        return true;
    }

private:
    CCLabelBMFont* m_clientIdStatus = nullptr;
    CCLabelBMFont* m_clientSecretStatus = nullptr;

    void updateCredentialStatus() {
        m_clientIdStatus->setString(state::clientId.empty()
            ? "Client ID: not set"
            : "Client ID: saved");
        m_clientSecretStatus->setString(state::clientSecret.empty()
            ? "Client secret: not set"
            : "Client secret: saved");
    }

    void openTutorial(CCObject*) {
        gui::openUrl("https://lynxdeer.com/autodeafen_setup.html");
    }

    void pasteClientId(CCObject*) {
        state::clientId = helpers::trimAsciiWhitespace(helpers::getClipboardText());
        Mod::get()->setSavedValue("CLIENT_ID", state::clientId);
        updateCredentialStatus();
    }

    void pasteClientSecret(CCObject*) {
        state::clientSecret = helpers::trimAsciiWhitespace(helpers::getClipboardText());
        Mod::get()->setSavedValue("CLIENT_SECRET", state::clientSecret);
        updateCredentialStatus();
    }

    void finishSetup(CCObject* sender) {
        state::clientId = helpers::trimAsciiWhitespace(state::clientId);
        state::clientSecret = helpers::trimAsciiWhitespace(state::clientSecret);

        if (state::clientId.empty() || state::clientSecret.empty()) {
            geode::createQuickPopup(
                "Discord Setup",
                "Paste both the Client ID and Client Secret before continuing.",
                "OK",
                "",
                [](auto, bool) {}
            );
            return;
        }

        if (!std::all_of(state::clientId.begin(), state::clientId.end(), [](unsigned char character) {
                return character >= '0' && character <= '9';
            })) {
            geode::createQuickPopup(
                "Discord Setup",
                "The Client ID should contain only numbers. Copy it from Discord's application settings and retry.",
                "OK",
                "",
                [](auto, bool) {}
            );
            return;
        }

        Mod::get()->setSavedValue("CLIENT_ID", state::clientId);
        Mod::get()->setSavedValue("CLIENT_SECRET", state::clientSecret);

        const auto stateToken = oauth::startServer(state::clientId, state::clientSecret);
        if (!stateToken) {
            geode::createQuickPopup(
                "Discord Setup",
                "AutoDeafen could not start its local authorization listener."
                " Close any existing setup window and try again.",
                "OK",
                "",
                [](auto, bool) {}
            );
            return;
        }

        const auto authorizationUrl =
            "https://discord.com/oauth2/authorize?client_id=" + helpers::formEncode(state::clientId) +
            "&response_type=code&redirect_uri=" + helpers::formEncode(oauth::kRedirectUri) +
            "&scope=rpc.voice.write+rpc&state=" + helpers::formEncode(*stateToken);

        gui::openUrl(authorizationUrl);

        onClose(sender);
    }
};

namespace gui {

inline void openAuthSetup() {
    if (auto layer = AuthLayer::create()) {
        layer->show();
    }
}

inline void openSetupPopup() {
    geode::createQuickPopup(
        "AutoDeafen",
        "AutoDeafen needs a Discord application before it can change your voice settings."
        " Open the guide to create one, then paste its Client ID and Client Secret here.",
        "Later",
        "Open Guide",
        [](auto, bool openedGuide) {
            if (!openedGuide) return;

            openAuthSetup();
            openUrl("https://lynxdeer.com/autodeafen_setup.html");
        }
    );
}

}

class ConfigLayer final : public geode::Popup {
public:
    static ConfigLayer* create() {
        auto layer = new ConfigLayer;
        if (layer && layer->init()) {
            layer->autorelease();
            return layer;
        }

        delete layer;
        return nullptr;
    }

    void onClose(CCObject* sender) override {
        if (m_percentageInput && !m_percentageInput->getString().empty()) {
            const auto fallback = Mod::get()->getSettingValue<float>("default_percentage");
            const auto value = geode::utils::numFromString<float>(m_percentageInput->getString())
                .ok()
                .value_or(fallback);
            state::deafenPercentage = std::clamp(value, 0.f, 100.f);
        }

        level_settings::save();

        Popup::onClose(sender);
    }

protected:
    bool init() override {
        if (!Popup::init(300.f, 200.f)) return false;

        setKeyboardEnabled(true);

        const auto topLeft = ccp(0.f, m_size.height);

        auto title = CCLabelBMFont::create("AutoDeafen", "goldFont.fnt");
        title->setPosition(topLeft + ccp(142.f, 5.f));

        auto enabledLabel = CCLabelBMFont::create("Enabled", "bigFont.fnt");
        enabledLabel->setAnchorPoint({0.f, 0.5f});
        enabledLabel->setScale(0.7f);
        enabledLabel->setPosition(topLeft + ccp(60.f, -60.f));

        auto enabledButton = CCMenuItemToggler::create(
            CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png"),
            CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png"),
            this,
            menu_selector(ConfigLayer::toggleEnabled)
        );
        enabledButton->setPosition(enabledLabel->getPosition() + ccp(140.f, 0.f));
        enabledButton->setScale(0.85f);
        enabledButton->toggle(state::deafenEnabled);

        auto percentageLabel = CCLabelBMFont::create("Percent", "bigFont.fnt");
        percentageLabel->setAnchorPoint({0.f, 0.5f});
        percentageLabel->setScale(0.7f);
        percentageLabel->setPosition(topLeft + ccp(60.f, -100.f));

        m_percentageInput = TextInput::create(100.f, "%");
        m_percentageInput->setCommonFilter(geode::CommonFilter::Float);
        m_percentageInput->setWidth(80.f);
        m_percentageInput->setPosition(enabledButton->getPosition() + ccp(0.f, -40.f));
        m_percentageInput->setScale(0.85f);
        m_percentageInput->setMaxCharCount(7);
        m_percentageInput->setString(gui::formatPercentage(state::deafenPercentage));

        auto setupButton = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Re-authenticate"),
            this,
            menu_selector(ConfigLayer::runSetup)
        );
        setupButton->setPosition(topLeft + ccp(142.f, -150.f));

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        menu->addChild(enabledButton);
        menu->addChild(m_percentageInput);
        menu->addChild(setupButton);

        m_mainLayer->addChild(title);
        m_mainLayer->addChild(enabledLabel);
        m_mainLayer->addChild(percentageLabel);
        m_mainLayer->addChild(menu);

        return true;
    }

private:
    TextInput* m_percentageInput = nullptr;

    void runSetup(CCObject*) {
        gui::openSetupPopup();
    }

    void toggleEnabled(CCObject*) {
        state::deafenEnabled = !state::deafenEnabled;
    }
};

namespace gui {

inline void openModPopup() {
    if (auto layer = ConfigLayer::create()) {
        layer->show();
    }
}

}
