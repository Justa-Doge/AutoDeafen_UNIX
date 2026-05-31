#include "globals.h"

std::string CLIENT_ID = "";
std::string CLIENT_SECRET = "";
std::string DISCORD_ACCESS_TOKEN = "";
std::string DISCORD_REFRESH_TOKEN = "";

bool webRequestDone = false;
bool resDidntReturnOk = false;

int responseCode = 0;
std::string responseError;
long long TOKEN_EXPIRY = 0;

bool currentlyInMenu = false;
bool DEAFEN_ENABLED = false;
float DEAFEN_PERCENTAGE = 50.0f;
bool deafenedThisAttempt = false;
bool hasDied = false;
std::string CURRENT_LEVEL = "";