#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <Unreal/Core/HAL/Platform.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/UnrealCoreStructs.hpp>
#include <Unreal/UFunctionStructs.hpp>

namespace RC::Unreal
{
    class UObject;
}

namespace PalCrosschat
{
    // Online audience for dual chat broadcasts (Steam/PS5 formatted, Xbox plain).
    //
    // Roster is event-driven (no periodic FindAllOf):
    //   - Bootstrap: one-shot SEH FindAllOf at first inject (players already online)
    //   - Join: PalPlayerState:RequestJoinPlayer_ToServer hook
    //   - Leave: FWeakObjectPtr on PlayerState goes invalid (swept on tick)
    //   - Chat: Upsert if missing / refresh platform
    //   - Inject fail targeting receivers: Forget those uids
    // All failure paths log and continue; never crash the server.
    class AudienceTracker
    {
    public:
        AudienceTracker() = default;
        ~AudienceTracker();

        AudienceTracker(const AudienceTracker&) = delete;
        AudienceTracker& operator=(const AudienceTracker&) = delete;

        void Register();
        void Unregister();

        // Game thread / on_update: pending joins + prune dead weak refs.
        void Tick();

        // One-shot SEH FindAllOf to seed players already online at restart.
        // Never runs again (join hook + chat Upsert cover the rest). Safe if it faults.
        void TryBootstrapOnce();

        // Chat / join / !setdiscord: add or refresh (creates if missing).
        void Upsert(const RC::Unreal::FGuid& player_uid,
                    const std::string& platform_user_id,
                    RC::Unreal::UObject* player_state = nullptr);

        void Remember(const RC::Unreal::FGuid& player_uid, const std::string& platform_user_id);

        void Forget(const RC::Unreal::FGuid& player_uid);
        void ForgetMany(const std::vector<RC::Unreal::FGuid>& player_uids);

        void Snapshot(std::vector<RC::Unreal::FGuid>& xbox,
                      std::vector<RC::Unreal::FGuid>& others);

        bool IsOnlinePlayerUid(const RC::Unreal::FGuid& player_uid) const;
        size_t Size() const;

    private:
        struct RosterEntry
        {
            RC::Unreal::FGuid uid{};
            std::string platform; // steam_ / gdk_ / ps5_ (may be empty until known)
            RC::Unreal::FWeakObjectPtr player_state{};
            bool has_weak = false; // true only after assigning a live PlayerState
        };

        struct PendingJoin
        {
            RC::Unreal::FWeakObjectPtr player_state{};
            int frames = 0;
        };

        static void OnRequestJoinPlayer(RC::Unreal::UnrealScriptFunctionCallableContext& context,
                                        void* custom_data);

        void EnqueueJoin(RC::Unreal::UObject* player_state);
        void ProcessPendingJoins();
        void RebuildListsUnlocked();
        bool TryReadJoinIdentity(RC::Unreal::UObject* player_state,
                                 RC::Unreal::FGuid& out_uid,
                                 std::string& out_platform) const;

        mutable std::mutex m_mutex;
        std::unordered_map<std::string, RosterEntry> m_roster;
        std::vector<PendingJoin> m_pending_joins;
        std::vector<RC::Unreal::FGuid> m_xbox;
        std::vector<RC::Unreal::FGuid> m_others;

        bool m_registered = false;
        bool m_bootstrapped = false;
        std::pair<int, int> m_hook_ids{-1, -1};
    };
}
