#include "globals.h"

namespace state {

std::string clientId;
std::string clientSecret;
std::string accessToken;
std::string refreshToken;
std::int64_t tokenExpiry = 0;

bool deafenEnabled = false;
float deafenPercentage = 50.0f;
bool deafenedThisAttempt = false;
bool playerDiedThisAttempt = false;
std::string currentLevelKey;

}
