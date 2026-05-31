#pragma once
#include <string>

extern std::string CLIENT_ID;
extern std::string CLIENT_SECRET;
extern std::string DISCORD_ACCESS_TOKEN;
extern std::string DISCORD_REFRESH_TOKEN;

extern bool webRequestDone;
extern bool resDidntReturnOk;

extern int responseCode;
extern std::string responseError;
extern long long TOKEN_EXPIRY;

extern bool currentlyInMenu;
extern bool DEAFEN_ENABLED;
extern float DEAFEN_PERCENTAGE;
extern bool deafenedThisAttempt;
extern bool hasDied;
extern std::string CURRENT_LEVEL;