#pragma once

#include "Config.h"
#include "Queues.h"

#include <Unreal/UFunctionStructs.hpp>

namespace PalCrosschat
{
    class ChatCapture
    {
    public:
        ChatCapture(const Config& config, OutboundQueue& outbound);

        // Call from on_unreal_init. Returns true if the post-hook registered.
        bool Register();

        void Unregister();

    private:
        static void OnEnterChatReceive(RC::Unreal::UnrealScriptFunctionCallableContext& context,
                                       void* custom_data);

        void HandleHook(RC::Unreal::UnrealScriptFunctionCallableContext& context);

        const Config& m_config;
        OutboundQueue& m_outbound;
        std::pair<int, int> m_hook_ids{-1, -1};
        bool m_registered = false;
    };
}
