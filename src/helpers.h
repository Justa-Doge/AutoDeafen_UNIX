#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace helpers {

enum class TokenRequestKind {
    AuthorizationCode,
    Refresh,
};

std::string getClipboardText();
std::int64_t currentTime();
std::string trimAsciiWhitespace(std::string_view value);
std::string formEncode(std::string_view value);

std::string tokenRequestBody(
    std::string_view clientId,
    std::string_view clientSecret,
    std::string_view grantType,
    std::string_view parameterName,
    std::string_view parameterValue
);

void initializeDiscordIpc();
void refreshDiscordAuthIfNeeded();
void sendTokenRequest(
    std::string requestBody,
    std::string clientId,
    std::string clientSecret,
    TokenRequestKind kind,
    std::string expectedRefreshToken = {}
);
void sendRefreshRequest();

}
