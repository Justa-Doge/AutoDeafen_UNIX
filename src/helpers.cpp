#include "helpers.h"

#include <Geode/Geode.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/utils/general.hpp>
#include <Geode/utils/web.hpp>

#include <chrono>
#include <deque>
#include <utility>
#include <vector>

#include "globals.h"
#include "ipc.h"
#include "oauth.h"

namespace {

namespace web = geode::utils::web;

constexpr auto kRefreshRetryDelay = std::chrono::seconds(30);
constexpr auto kExpiryCheckInterval = std::chrono::seconds(1);
constexpr std::int64_t kRefreshSafetyMarginSeconds = 60;

const std::string& discordUserAgent() {
    static const std::string userAgent =
        "AutoDeafen (https://github.com/Justa-Doge/AutoDeafen_UNIX, " +
        geode::Mod::get()->getVersion().toNonVString() + ")";
    return userAgent;
}

struct TokenRequest {
    std::string body;
    std::string clientId;
    std::string clientSecret;
    helpers::TokenRequestKind kind = helpers::TokenRequestKind::AuthorizationCode;
    std::string expectedRefreshToken;
};

struct TokenRequestQueue {
    std::deque<TokenRequest> pending;
    geode::async::TaskHolder<web::WebResponse> task;
    bool active = false;
    bool refreshPending = false;
    std::chrono::steady_clock::time_point nextRefreshAttempt {};
};

TokenRequestQueue& tokenRequests() {
    static TokenRequestQueue queue;
    return queue;
}

void beginNextTokenRequest();

void showTokenFailure(helpers::TokenRequestKind kind, std::string message) {
    if (kind != helpers::TokenRequestKind::AuthorizationCode) return;

    geode::createQuickPopup(
        "Discord Setup",
        message,
        "OK",
        "",
        [](auto, bool) {}
    );
}

bool hasScope(std::string_view scopes, std::string_view expected) {
    while (!scopes.empty()) {
        const auto separator = scopes.find(' ');
        if (scopes.substr(0, separator) == expected) return true;
        if (separator == std::string_view::npos) break;
        scopes.remove_prefix(separator + 1);
    }
    return false;
}

std::string tokenFailureMessage(
    const web::WebResponse& response,
    std::string_view discordError
) {
    if (discordError == "invalid_client") {
        return "Discord rejected the Client ID or Client Secret. Copy both values again and retry.";
    }
    if (discordError == "invalid_grant") {
        return "Discord rejected or expired the authorization code. Confirm that the application's redirect is exactly http://localhost:8000, then retry.";
    }
    if (discordError == "invalid_scope") {
        return "Discord did not allow the RPC voice scopes for this application. Check the application's RPC access and tester settings.";
    }
    if (response.error() || response.code() <= 0) {
        return "AutoDeafen could not reach Discord's token service. Check the connection and retry.";
    }

    return "Discord could not finish authorization (HTTP " +
        std::to_string(response.code()) + "). Check the Geode log for the reason.";
}

void handleTokenResponse(
    web::WebResponse response,
    const std::string& requestClientId,
    const std::string& requestClientSecret,
    helpers::TokenRequestKind kind,
    const std::string& expectedRefreshToken
) {
    if (requestClientId != state::clientId || requestClientSecret != state::clientSecret) {
        geode::prelude::log::warn("Ignoring an OAuth response for credentials that have since changed");
        return;
    }

    if (kind == helpers::TokenRequestKind::Refresh &&
        expectedRefreshToken != state::refreshToken) {
        geode::prelude::log::info("Ignoring a stale Discord token refresh response");
        return;
    }

    const auto json = response.json().unwrapOr(matjson::Value());
    const auto discordError = json["error"].asString().unwrapOr("");
    const auto discordDescription = json["error_description"].asString().unwrapOr("");

    if (!response.ok()) {
        if (!discordError.empty() || !discordDescription.empty()) {
            geode::prelude::log::warn(
                "Discord OAuth token exchange failed (HTTP {}): {}{}{}",
                response.code(),
                discordError.empty() ? "unknown_error" : discordError,
                discordDescription.empty() ? "" : " - ",
                discordDescription
            );
        } else {
            geode::prelude::log::warn(
                "Discord OAuth token exchange failed (HTTP {}): {}",
                response.code(),
                response.errorMessage().empty() ? "no error details returned" : response.errorMessage()
            );
        }
        showTokenFailure(kind, tokenFailureMessage(response, discordError));
        return;
    }

    const auto accessToken = json["access_token"].asString().unwrapOr("");
    const auto refreshToken = json["refresh_token"].asString().unwrapOr("");
    const auto expiresIn = json["expires_in"].asInt().unwrapOr(0);

    const auto requiresRefreshToken = kind == helpers::TokenRequestKind::AuthorizationCode;
    if (accessToken.empty() || (requiresRefreshToken && refreshToken.empty()) || expiresIn <= 0) {
        geode::prelude::log::warn(
            "Discord returned an incomplete OAuth response (access token: {}, refresh token: {}, expiry: {})",
            accessToken.empty() ? "missing" : "present",
            refreshToken.empty() ? "missing" : "present",
            expiresIn > 0 ? "present" : "missing"
        );
        showTokenFailure(
            kind,
            "Discord returned an incomplete authorization response. Retry and check the Geode log if it continues."
        );
        return;
    }

    const auto grantedScopes = json["scope"].asString().unwrapOr("");
    if (!grantedScopes.empty() &&
        (!hasScope(grantedScopes, "rpc") || !hasScope(grantedScopes, "rpc.voice.write"))) {
        geode::prelude::log::warn(
            "Discord did not grant the required RPC scopes (granted: {})",
            grantedScopes
        );
        showTokenFailure(
            kind,
            "Discord did not grant RPC voice access. Check the application's RPC access and tester settings, then retry."
        );
        return;
    }

    state::accessToken = accessToken;
    if (!refreshToken.empty()) {
        state::refreshToken = refreshToken;
    }
    state::tokenExpiry = helpers::currentTime() + expiresIn;

    auto mod = geode::prelude::Mod::get();
    mod->setSavedValue("DISCORD_ACCESS_TOKEN", state::accessToken);
    mod->setSavedValue("DISCORD_REFRESH_TOKEN", state::refreshToken);
    mod->setSavedValue("TOKEN_EXPIRY", state::tokenExpiry);

    geode::prelude::log::info("Discord authentication updated");
    ipc::initializeDiscordAuth(requestClientId, accessToken);
}

void beginNextTokenRequest() {
    auto& queue = tokenRequests();
    if (queue.active) return;

    while (!queue.pending.empty() &&
        queue.pending.front().kind == helpers::TokenRequestKind::Refresh &&
        queue.pending.front().expectedRefreshToken != state::refreshToken) {
        queue.pending.pop_front();
        queue.refreshPending = false;
        geode::prelude::log::info("Discarding a stale Discord token refresh request");
    }

    if (queue.pending.empty()) return;

    auto pending = std::move(queue.pending.front());
    queue.pending.pop_front();
    queue.active = true;

    auto request = web::WebRequest();
    request.userAgent(discordUserAgent());
    request.header("Content-Type", "application/x-www-form-urlencoded");
    request.body(std::vector<std::uint8_t>(pending.body.begin(), pending.body.end()));
    request.timeout(std::chrono::seconds(20));

    queue.task.spawn(
        request.post("https://discord.com/api/oauth2/token"),
        [
            clientId = std::move(pending.clientId),
            clientSecret = std::move(pending.clientSecret),
            kind = pending.kind,
            expectedRefreshToken = std::move(pending.expectedRefreshToken)
        ](web::WebResponse response) {
            handleTokenResponse(
                std::move(response),
                clientId,
                clientSecret,
                kind,
                expectedRefreshToken
            );

            auto& queue = tokenRequests();
            if (kind == helpers::TokenRequestKind::Refresh) {
                queue.refreshPending = false;
            }
            queue.active = false;
            beginNextTokenRequest();
        }
    );
}

}

std::string helpers::getClipboardText() {
    return geode::utils::clipboard::read();
}

std::int64_t helpers::currentTime() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

std::string helpers::trimAsciiWhitespace(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};

    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

std::string helpers::formEncode(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";

    std::string encoded;
    encoded.reserve(value.size());

    for (const auto byte : value) {
        const auto character = static_cast<unsigned char>(byte);
        const bool isUnreserved =
            (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            character == '-' || character == '_' ||
            character == '.' || character == '~';

        if (isUnreserved) {
            encoded += static_cast<char>(character);
        } else {
            encoded += '%';
            encoded += hex[character >> 4];
            encoded += hex[character & 0x0F];
        }
    }

    return encoded;
}

std::string helpers::tokenRequestBody(
    std::string_view clientId,
    std::string_view clientSecret,
    std::string_view grantType,
    std::string_view parameterName,
    std::string_view parameterValue
) {
    return "client_id=" + formEncode(clientId) +
        "&client_secret=" + formEncode(clientSecret) +
        "&grant_type=" + formEncode(grantType) +
        "&" + std::string(parameterName) + "=" + formEncode(parameterValue) +
        "&redirect_uri=" + formEncode(oauth::kRedirectUri);
}

void helpers::initializeDiscordIpc() {
    ipc::initializeDiscordAuth(state::clientId, state::accessToken);
}

void helpers::refreshDiscordAuthIfNeeded() {
    static auto nextCheck = std::chrono::steady_clock::time_point {};
    const auto now = std::chrono::steady_clock::now();
    if (now < nextCheck) return;
    nextCheck = now + kExpiryCheckInterval;

    if (state::tokenExpiry <= 0 ||
        currentTime() + kRefreshSafetyMarginSeconds < state::tokenExpiry) {
        return;
    }

    sendRefreshRequest();
}

void helpers::sendTokenRequest(
    std::string requestBody,
    std::string clientId,
    std::string clientSecret,
    TokenRequestKind kind,
    std::string expectedRefreshToken
) {
    geode::queueInMainThread([
        request = TokenRequest {
            std::move(requestBody),
            std::move(clientId),
            std::move(clientSecret),
            kind,
            std::move(expectedRefreshToken),
        }
    ]() mutable {
        auto& queue = tokenRequests();
        if (request.kind == TokenRequestKind::Refresh) {
            const auto now = std::chrono::steady_clock::now();
            if (queue.refreshPending || now < queue.nextRefreshAttempt) return;

            queue.refreshPending = true;
            queue.nextRefreshAttempt = now + kRefreshRetryDelay;
        }

        queue.pending.push_back(std::move(request));
        beginNextTokenRequest();
    });
}

void helpers::sendRefreshRequest() {
    const auto clientId = state::clientId;
    const auto clientSecret = state::clientSecret;
    const auto refreshToken = state::refreshToken;
    if (clientId.empty() || clientSecret.empty() || refreshToken.empty()) return;

    sendTokenRequest(
        tokenRequestBody(
            clientId,
            clientSecret,
            "refresh_token",
            "refresh_token",
            refreshToken
        ),
        clientId,
        clientSecret,
        TokenRequestKind::Refresh,
        refreshToken
    );
}
