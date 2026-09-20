#pragma once

#include <string>

namespace ipc {

void initializeDiscordAuth(std::string clientId, std::string accessToken);
void setDeafened(bool deafened);

}
