#pragma once

#include "Audience.h"
#include "Config.h"
#include "Queues.h"
#include "Webhook.h"
#include "WordFilter.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

#include <Unreal/Core/HAL/Platform.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/UnrealCoreStructs.hpp>
#include <Unreal/UFunctionStructs.hpp>

namespace PalCrosschat
{
    class ChatInject;

    class ChatCapture
    {
    public:
        ChatCapture(const Config& config,
                    OutboundQueue& outbound,
                    LinkQueue& link_jobs,
                    WebhookWorker* webhook,
                    WordFilter* filter,
                    ChatInject* inject,
                    AudienceTracker* audience);

        // Call from on_unreal_init. Returns true if the pre-hook registered.
        bool Register();

        void Unregister();

        // Resolve deferred !setdiscord work (UniqueNetId ProcessEvent). Game thread / on_update only.
        void FlushDeferred();

        // Private chat-line feedback for !setdiscord (ReceiverPlayerUIds).
        void DeliverLinkResult(const LinkResult& result);

    private:
        struct PendingSetDiscord
        {
            RC::Unreal::FWeakObjectPtr controller{};
            std::string connect_code;
        };

        // Chat relay deferred so GetPlayerName ProcessEvent runs outside EnterChat_Receive.
        struct PendingChatRelay
        {
            RC::Unreal::FWeakObjectPtr controller{};
            std::string message;
            std::string guild_name;
            std::string sender_id;
            RC::Unreal::FGuid sender_uid{};
            uint8_t category = 0;
            bool local_tag = false;
        };

        struct LinkClient
        {
            RC::Unreal::FWeakObjectPtr controller{};
            RC::Unreal::FGuid player_uid{};
        };

        std::string LookupCachedPlayerName(const std::string& uid_key) const;
        void CachePlayerName(const std::string& uid_key, const std::string& name);
        void FlushPendingChatRelays();

        static void OnEnterChatReceive(RC::Unreal::UnrealScriptFunctionCallableContext& context,
                                       void* custom_data);

        void HandleHook(RC::Unreal::UnrealScriptFunctionCallableContext& context);
        void ProcessSetDiscord(RC::Unreal::UObject* controller, const std::string& connect_code);

        const Config& m_config;
        OutboundQueue& m_outbound;
        LinkQueue& m_link_jobs;
        WebhookWorker* m_webhook = nullptr;
        WordFilter* m_filter = nullptr;
        ChatInject* m_inject = nullptr;
        AudienceTracker* m_audience = nullptr;
        std::pair<int, int> m_hook_ids{-1, -1};
        bool m_registered = false;

        std::mutex m_pending_mutex;
        std::deque<PendingSetDiscord> m_pending_setdiscord;
        std::deque<PendingChatRelay> m_pending_chat_relays;

        std::mutex m_link_controllers_mutex;
        std::unordered_map<std::string, LinkClient> m_link_clients;

        // PlayerUId guid string -> steam_/gdk_/ps5_ (avoids UniqueNetId ProcessEvent on retries).
        std::mutex m_platform_cache_mutex;
        std::unordered_map<std::string, std::string> m_platform_by_player_uid;

        // PlayerUId -> display name from GetPlayerName (PE only outside EnterChat_Receive).
        mutable std::mutex m_name_cache_mutex;
        std::unordered_map<std::string, std::string> m_name_by_player_uid;
    };
}
