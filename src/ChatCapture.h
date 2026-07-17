#pragma once

#include "Config.h"
#include "Queues.h"
#include "Webhook.h"
#include "WordFilter.h"

#include <memory>

#include <Unreal/UFunctionStructs.hpp>

namespace PalCrosschat
{
    class ChatCapture
    {
    public:
        ChatCapture(const Config& config, OutboundQueue& outbound, WebhookWorker* webhook);

        // Call from on_unreal_init. Returns true if the pre-hook registered.
        bool Register();

        void Unregister();

    private:
        static void OnEnterChatReceive(RC::Unreal::UnrealScriptFunctionCallableContext& context,
                                       void* custom_data);

        void HandleHook(RC::Unreal::UnrealScriptFunctionCallableContext& context);

        const Config& m_config;
        OutboundQueue& m_outbound;
        WebhookWorker* m_webhook = nullptr;
        std::unique_ptr<WordFilter> m_filter;
        std::pair<int, int> m_hook_ids{-1, -1};
        bool m_registered = false;
    };
}
