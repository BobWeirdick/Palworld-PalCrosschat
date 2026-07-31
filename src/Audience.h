#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Platform.hpp defines uint8 before UnrealCoreStructs uses it.
#include <Unreal/Core/HAL/Platform.hpp>
#include <Unreal/UnrealCoreStructs.hpp>

namespace PalCrosschat
{
    // Tracks PlayerUId -> platform (steam_/gdk_/ps5_) and splits online players into
    // Xbox (gdk_) vs everyone else for dual chat broadcasts.
    class AudienceTracker
    {
    public:
        // Cache a player's platform from chat / !setdiscord (game thread or any).
        void Remember(const RC::Unreal::FGuid& player_uid, const std::string& platform_user_id);

        // Resolve unknown platforms via AccountName / SEH UniqueNetId (game thread,
        // outside EnterChat_Receive). Cap work per call.
        void WarmUnknownPlatforms(int max_resolves = 4);

        // Scan online PalPlayerState objects (game thread only).
        // xbox = gdk_ only; others = steam_/ps5_/unknown (formatted nil-UID path).
        void Collect(std::vector<RC::Unreal::FGuid>& xbox,
                     std::vector<RC::Unreal::FGuid>& others);

        // True if this PlayerUId belongs to a currently online PalPlayerState.
        bool IsOnlinePlayerUid(const RC::Unreal::FGuid& player_uid) const;

    private:
        std::mutex m_mutex;
        // FormatGuid(PlayerUId) -> steam_/gdk_/ps5_…
        std::unordered_map<std::string, std::string> m_platform_by_uid;
    };
}
