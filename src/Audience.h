#pragma once

#include <chrono>
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
    // Online audience for dual chat broadcasts.
    //
    // CRITICAL: FindAllOf must NOT run on every chat line — that pattern AVs inside
    // UE4SS (same class as ServerEventsRelay crashes). Refresh runs on a slow timer
    // from on_update; BroadcastDual only reads the snapshot. No UniqueNetId PE.
    class AudienceTracker
    {
    public:
        // Cache platform from chat / !setdiscord (any thread).
        void Remember(const RC::Unreal::FGuid& player_uid, const std::string& platform_user_id);

        // Game thread / on_update only. At most one FindAllOf per interval; AccountName only.
        void TickRefresh();

        // Copy last snapshot (no FindAllOf). xbox = gdk_; others = everyone else online.
        void Snapshot(std::vector<RC::Unreal::FGuid>& xbox,
                      std::vector<RC::Unreal::FGuid>& others) const;

        // True if uid was in the last successful refresh (no FindAllOf).
        bool IsOnlinePlayerUid(const RC::Unreal::FGuid& player_uid) const;

    private:
        void RefreshNow();

        mutable std::mutex m_mutex;
        // FormatGuid(PlayerUId) -> steam_/gdk_/ps5_…
        std::unordered_map<std::string, std::string> m_platform_by_uid;
        std::vector<RC::Unreal::FGuid> m_xbox;
        std::vector<RC::Unreal::FGuid> m_others;
        std::unordered_set<std::string> m_online_keys;
        std::chrono::steady_clock::time_point m_last_refresh{};
        bool m_have_snapshot = false;
    };
}
