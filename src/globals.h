#pragma once
#include <string>

inline std::string CLIENT_ID = "";
inline std::string CLIENT_SECRET = "";
inline std::string DISCORD_ACCESS_TOKEN = "";
inline std::string DISCORD_REFRESH_TOKEN = "";

inline bool webRequestDone = false;
inline bool resDidntReturnOk = false;

inline int responseCode = 0;
inline std::string responseError;
inline long long TOKEN_EXPIRY = 0;


inline bool currentlyInMenu = false;
inline bool DEAFEN_ENABLED = false;
inline float DEAFEN_PERCENTAGE = 50.0f;
inline bool deafenedThisAttempt = false;
inline bool hasDied = false;
inline std::string CURRENT_LEVEL = "";

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