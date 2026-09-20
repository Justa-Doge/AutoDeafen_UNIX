#include "linux_setup.h"

#include <Geode/Geode.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/platform/cplatform.h>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/utils/general.hpp>
#include <Geode/utils/string.hpp>
#include <Geode/utils/web.hpp>

#if defined(GEODE_IS_WINDOWS)
    #include <Geode/loader/GameEvent.hpp>

    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <shellapi.h>
#endif

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr std::string_view kGuideFilename = "LINUX_SETUP.txt";
constexpr std::string_view kSettingType = "linux-guide";
constexpr std::string_view kSettingKey = "linux_setup_guide";
constexpr std::string_view kOpenButtonKey = "open";
constexpr std::string_view kOpenLogFolderButtonKey = "open-log-folder";
constexpr std::string_view kPromptShownKey = "LINUX_SETUP_PROMPT_SHOWN";

#if defined(GEODE_IS_WINDOWS)

class HiddenSettingNode final : public geode::SettingNodeV3 {
protected:
    bool init(std::shared_ptr<geode::SettingV3> setting, float width) {
        if (!SettingNodeV3::init(std::move(setting), width)) return false;

        setVisible(false);
        setContentSize({width, 0.f});
        return true;
    }

    void onCommit() override {}
    void onResetToDefault() override {}

public:
    static HiddenSettingNode* create(
        std::shared_ptr<geode::SettingV3> setting,
        float width
    ) {
        auto node = new HiddenSettingNode;
        if (node && node->init(std::move(setting), width)) {
            node->autorelease();
            return node;
        }

        delete node;
        return nullptr;
    }

    bool hasUncommittedChanges() const override {
        return false;
    }

    bool hasNonDefaultValue() const override {
        return false;
    }
};

class HiddenSetting final : public geode::SettingV3 {
public:
    HiddenSetting() = default;

    static geode::Result<std::shared_ptr<geode::SettingV3>> parse(
        std::string key,
        std::string modId,
        const matjson::Value& json
    ) {
        auto setting = std::make_shared<HiddenSetting>();
        GEODE_UNWRAP(setting->parseBaseProperties(
            std::move(key),
            std::move(modId),
            json
        ));
        return geode::Ok(std::static_pointer_cast<geode::SettingV3>(setting));
    }

    bool load(const matjson::Value&) override {
        return true;
    }

    bool save(matjson::Value&) const override {
        return true;
    }

    geode::SettingNodeV3* createNode(float width) override {
        return HiddenSettingNode::create(shared_from_this(), width);
    }

    bool isDefaultValue() const override {
        return true;
    }

    void reset() override {}
};

geode::Result<std::shared_ptr<geode::SettingV3>> createLinuxGuideSetting(
    std::string key,
    std::string modId,
    const matjson::Value& json
) {
    if (!linux_setup::isLinuxHost()) {
        return HiddenSetting::parse(std::move(key), std::move(modId), json);
    }

    GEODE_UNWRAP_INTO(
        auto setting,
        geode::ButtonSettingV3::parse(std::move(key), std::move(modId), json)
    );
    return geode::Ok(std::static_pointer_cast<geode::SettingV3>(std::move(setting)));
}

#endif

}

bool linux_setup::isLinuxHost() {
#if defined(GEODE_IS_WINDOWS)
    static const bool detected = [] {
        if (!geode::utils::platform::isWine()) return false;

        const auto ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) return false;

        using GetWineHostVersion = void (__cdecl*)(const char**, const char**);
        const auto getHostVersion = reinterpret_cast<GetWineHostVersion>(
            GetProcAddress(ntdll, "wine_get_host_version")
        );
        if (!getHostVersion) {
            geode::log::warn(
                "Wine did not expose wine_get_host_version; the Linux setup guide will stay hidden"
            );
            return false;
        }

        const char* systemName = nullptr;
        getHostVersion(&systemName, nullptr);
        return systemName && std::string_view(systemName) == "Linux";
    }();
    return detected;
#else
    return false;
#endif
}

void linux_setup::registerSetting() {
#if defined(GEODE_IS_WINDOWS)
    const auto registration = geode::Mod::get()->registerCustomSettingType(
        kSettingType,
        createLinuxGuideSetting
    );
    if (!registration) {
        geode::log::error(
            "Could not register the Linux setup setting: {}",
            registration.unwrapErr()
        );
        return;
    }

    geode::ButtonSettingPressedEventV3(
        geode::Mod::get(),
        std::string(kSettingKey)
    ).listen([](std::string_view buttonKey) {
        if (buttonKey == kOpenButtonKey) {
            linux_setup::openGuide();
        } else if (buttonKey == kOpenLogFolderButtonKey) {
            linux_setup::openBridgeLogFolder();
        }
    }).leak();
#endif
}

void linux_setup::openBridgeLogFolder() {
#if defined(GEODE_IS_WINDOWS)
    if (!isLinuxHost()) return;

    const std::filesystem::path logDirectory = LR"(C:\windows\logs)";
    std::error_code error;
    if (!std::filesystem::is_directory(logDirectory, error)) {
        geode::log::warn(
            "[ADIPC-BRIDGE-LOG-FOLDER] area=openBridgeLogFolder result=missing "
            "path=C:\\windows\\logs error={}",
            error ? error.message() : "directory does not exist"
        );
        geode::Notification::create(
            "Bridge log folder not found. Launch the AutoDeafen IPC bridge first.",
            geode::NotificationIcon::Warning,
            geode::NOTIFICATION_LONG_TIME
        )->show();
        return;
    }

    // Geode's Win64 openFolder implementation intentionally uses Windows
    // Explorer. Under Wine, winebrowser converts this prefix-local Windows path
    // to its Linux path and hands it to the desktop's default opener instead.
    const auto result = reinterpret_cast<std::intptr_t>(ShellExecuteW(
        nullptr,
        L"open",
        L"winebrowser.exe",
        L"\"C:\\windows\\logs\"",
        nullptr,
        SW_SHOWNORMAL
    ));
    if (result <= 32) {
        geode::log::warn(
            "[ADIPC-BRIDGE-LOG-FOLDER] area=openBridgeLogFolder result=open-failed "
            "shell-error={}",
            result
        );
        geode::Notification::create(
            "Could not open the bridge log folder in the Linux file manager.",
            geode::NotificationIcon::Error,
            geode::NOTIFICATION_LONG_TIME
        )->show();
        return;
    }

    geode::log::info(
        "[ADIPC-BRIDGE-LOG-FOLDER] area=openBridgeLogFolder result=opened"
    );
#endif
}

void linux_setup::showSetupPrompt() {
    if (!isLinuxHost()) return;

    geode::createQuickPopup(
        "Linux Setup",
        "Using native Linux Discord? AutoDeafen needs its multi-IPC bridge for "
        "native packages, Flatpak, or Snap.\n\n"
        "Would you like to open the setup guide now?",
        "No",
        "Open",
        [](auto, bool shouldOpen) {
            if (shouldOpen) linux_setup::openGuide();
        }
    );
}

void linux_setup::maybeShowFirstRunPrompt() {
    if (!isLinuxHost()) return;

    auto* mod = geode::Mod::get();
    if (mod->getSavedValue<bool>(kPromptShownKey, false)) return;

    // Record this before opening the popup so returning to the menu cannot
    // queue a second copy.
    mod->setSavedValue(kPromptShownKey, true);
    (void)mod->saveData();

    showSetupPrompt();
}

void linux_setup::openGuide() {
    if (!isLinuxHost()) return;

    const auto guidePath = geode::Mod::get()->getResourcesDir() / kGuideFilename;
    std::error_code error;
    if (!std::filesystem::is_regular_file(guidePath, error)) {
        geode::log::error(
            "The packaged Linux setup guide is missing at {}",
            geode::utils::string::pathToString(guidePath)
        );
        geode::createQuickPopup(
            "Linux Setup Guide",
            "The packaged guide file is missing. Reinstall AutoDeafen and try again.",
            "OK",
            "",
            [](auto, bool) {}
        );
        return;
    }

    // The path comes from AutoDeafen's own package, so opening it through the
    // platform file association is safe. Wine supplies a .txt viewer even when
    // the Linux desktop has no association forwarded into the prefix.
    geode::utils::web::openLinkUnsafe(
        geode::utils::string::pathToString(guidePath)
    );
}

#if defined(GEODE_IS_WINDOWS)
    $on_game(Loaded) {
        linux_setup::maybeShowFirstRunPrompt();
    }
#endif
