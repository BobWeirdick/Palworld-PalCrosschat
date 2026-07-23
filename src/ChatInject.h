#pragma once

#include "Config.h"
#include "Queues.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

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
        ChatInject(const Config& config, WordFilter* filter);
        ~ChatInject() = default;

        // Drain deferred hook work, then up to MaxBroadcastsPerTick from inbound.
        // Call only from on_update (game thread).
        void Drain(InboundQueue& inbound, int max_per_tick);

        // Queue a local Global rebroadcast with this server's ChatFormat prefix.
        // Safe to call from EnterChat_Receive (no ProcessEvent here).
        void EnqueueLocalTagged(const std::string& sender_name,
                                const std::string& guild_name,
                                const std::string& message,
                                uint8_t category);

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
            RC::Unreal::FGuid receiver_uid{};
            bool has_receiver = false;
        };

        RC::Unreal::UObject* ResolveGameState();
        bool EnsureBroadcastFunction();
        bool EnsureServerNoticeFunction();
        void FlushDeferred(int max_actions);
        bool BroadcastOne(const InboundMessage& msg);
        bool BroadcastDisplay(const std::string& display_sender,
                              const std::string& message,
                              uint8_t category,
                              const RC::Unreal::FGuid* receiver_only = nullptr);
        bool BroadcastLocalTagged(const std::string& sender_name,
                                  const std::string& guild_name,
                                  const std::string& message,
                                  uint8_t category);
        bool ShowServerNotice(const std::string& notice_message);
        bool SendScreenLog(RC::Unreal::UObject* player_controller, const std::string& message);

        std::string PrefixForOrigin(const std::string& origin) const;
        std::string LocalServerPrefix() const;

        const Config& m_config;
        WordFilter* m_filter = nullptr;
        RC::Unreal::FWeakObjectPtr m_game_state{};
        RC::Unreal::UFunction* m_broadcast_fn = nullptr;
        RC::Unreal::UFunction* m_server_notice_fn = nullptr;

        std::mutex m_deferred_mutex;
        std::deque<DeferredAction> m_deferred;

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
