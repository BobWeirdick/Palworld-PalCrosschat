#pragma once

#include "Config.h"
#include "Queues.h"
#include "Webhook.h"
#include "WordFilter.h"

#include <cstdint>
#include <string>

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
                    ChatInject* inject);

        // Call from on_unreal_init. Returns true if the pre-hook registered.
        bool Register();

        void Unregister();

    private:
        static void OnEnterChatReceive(RC::Unreal::UnrealScriptFunctionCallableContext& context,
                                       void* custom_data);

        void HandleHook(RC::Unreal::UnrealScriptFunctionCallableContext& context);

        const Config& m_config;
        OutboundQueue& m_outbound;
        LinkQueue& m_link_jobs;
        WebhookWorker* m_webhook = nullptr;
        WordFilter* m_filter = nullptr;
        ChatInject* m_inject = nullptr;
        std::pair<int, int> m_hook_ids{-1, -1};
        bool m_registered = false;
    };
}
