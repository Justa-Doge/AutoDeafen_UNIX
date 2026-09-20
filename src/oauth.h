#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace oauth {

inline constexpr std::uint16_t kCallbackPort = 8000;
inline constexpr std::string_view kRedirectUri = "http://localhost:8000";

std::optional<std::string> startServer(std::string clientId, std::string clientSecret);

}
