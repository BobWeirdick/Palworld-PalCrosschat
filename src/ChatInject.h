#pragma once

#include "Audience.h"
#include "Config.h"
#include "Queues.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <Unreal/Core/HAL/Platform.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/UnrealCoreStructs.hpp>

namespace RC::Unreal
{
    class UObject;
    class UFunction;
    class UStruct;
}

namespace PalCrosschat
{
    class WordFilter;

    class ChatInject
    {
    public:
        ChatInject(const Config& config, WordFilter* filter, AudienceTracker* audience);
        ~ChatInject() = default;

        // Drain deferred hook work, then up to MaxBroadcastsPerTick from inbound.
        // Call only from on_update (game thread).
        void Drain(InboundQueue& inbound, int max_per_tick);

        // Queue a local rebroadcast with this server's ChatFormat prefix.
        // Safe to call from EnterChat_Receive (no ProcessEvent here).
        void EnqueueLocalTagged(const std::string& sender_name,
                                const std::string& guild_name,
                                const std::string& message,
                                uint8_t category,
                                const RC::Unreal::FGuid& sender_player_uid);

        // Queue red server-notice banner (BroadcastServerNotice). Game-thread flush.
        void EnqueueServerNotice(const std::string& notice_message);

        // Queue private screen-log on one PlayerController. Stores a weak ref.
        void EnqueueScreenLog(RC::Unreal::UObject* player_controller, const std::string& message);

        // Queue a chat line visible only to one player (ReceiverPlayerUIds). Shows in chat history.
        void EnqueuePrivateChat(const RC::Unreal::FGuid& receiver_player_uid,
                                const std::string& message);

    private:
        enum class DeferredKind : uint8_t
        {
            LocalTagged,
            ServerNotice,
            ScreenLog,
            PrivateChat,
        };

        struct DeferredAction
        {
            DeferredKind kind = DeferredKind::ServerNotice;
            std::string sender_name;
            std::string guild_name;
            std::string message;
            uint8_t category = 0;
            RC::Unreal::FWeakObjectPtr controller{};
            // Empty = broadcast to everyone.
            std::vector<RC::Unreal::FGuid> receivers{};
            RC::Unreal::FGuid sender_uid{};
        };

        RC::Unreal::UObject* ResolveGameState();
        bool EnsureBroadcastFunction();
        bool EnsureServerNoticeFunction();
        // World + GameState present, and warmup elapsed (avoids startup ProcessEvent storms).
        bool CanInjectNow(int& out_max_this_tick, int configured_max);
        void FlushDeferred(int max_actions);
        bool BroadcastOne(const InboundMessage& msg);
        bool BroadcastDisplay(const std::string& display_sender,
                              const std::string& message,
                              uint8_t category,
                              const std::vector<RC::Unreal::FGuid>* receivers = nullptr,
                              const RC::Unreal::FGuid* sender_player_uid = nullptr);
        bool BroadcastLocalTagged(const std::string& sender_name,
                                  const std::string& guild_name,
                                  const std::string& message,
                                  uint8_t category,
                                  const RC::Unreal::FGuid& sender_player_uid);
        // Formatted+nil uid to steam_/ps5_; plain body+real uid to Xbox/unknown when present.
        bool BroadcastDual(const std::string& xbox_sender,
                           const std::string& xbox_message,
                           const std::string& formatted_sender,
                           const std::string& formatted_message,
                           uint8_t category,
                           const RC::Unreal::FGuid* sender_player_uid);
        bool ShowServerNotice(const std::string& notice_message);
        bool SendScreenLog(RC::Unreal::UObject* player_controller, const std::string& message);

        std::string PrefixForOrigin(const std::string& origin) const;
        std::string LocalServerPrefix() const;

        const Config& m_config;
        WordFilter* m_filter = nullptr;
        AudienceTracker* m_audience = nullptr;
        RC::Unreal::FWeakObjectPtr m_game_state{};
        RC::Unreal::UFunction* m_broadcast_fn = nullptr;
        RC::Unreal::UFunction* m_server_notice_fn = nullptr;

        std::mutex m_deferred_mutex;
        std::deque<DeferredAction> m_deferred;

        // Set on first World+GameState sighting; inject blocked until warmup elapses.
        std::chrono::steady_clock::time_point m_world_ready_at{};
        bool m_world_ready_logged = false;
        bool m_inject_enabled_logged = false;

        // After BroadcastChatMessage ProcessEvent AVs, stop all inject for this process
        // (retry storms were killing the dedicated server after the first fault).
        bool m_inject_circuit_open = false;
        void TripInjectCircuit(const TCHAR* reason);

        // Reflected offsets inside the ChatMessage struct parameter (relative to struct start).
        bool m_offsets_ready = false;
        int32_t m_chat_param_offset = 0;
        const RC::Unreal::UStruct* m_chat_struct = nullptr;
        int32_t m_off_category = -1;
        int32_t m_off_sender = -1;
        int32_t m_off_sender_uid = -1;
        int32_t m_off_message = -1;
        int32_t m_off_receivers = -1;
        int32_t m_off_message_id = -1;
        int32_t m_off_arg_keys = -1;
        int32_t m_off_arg_values = -1;
    };
}
