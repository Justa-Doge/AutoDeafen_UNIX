#pragma once

#include <cstdint>
#include <string>

namespace state {

extern std::string clientId;
extern std::string clientSecret;
extern std::string accessToken;
extern std::string refreshToken;
extern std::int64_t tokenExpiry;

extern bool deafenEnabled;
extern float deafenPercentage;
extern bool deafenedThisAttempt;
extern bool playerDiedThisAttempt;
extern std::string currentLevelKey;

}
