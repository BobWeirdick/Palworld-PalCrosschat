#pragma once

#include <string>

namespace RC::Unreal
{
    class UObject;
}

namespace PalCrosschat
{
    // Normalize AccountName / UniqueNetId into steam_/gdk_/ps5_ form.
    std::string NormalizePlatformUserId(std::string raw);

    // Property AccountName first; UniqueNetId ProcessEvent under SEH as fallback.
    // Call only on the game thread outside EnterChat_Receive.
    std::string ResolvePlatformUserId(RC::Unreal::UObject* player_state);
}
