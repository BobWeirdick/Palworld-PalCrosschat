#pragma once

#include "Config.h"
#include "Queues.h"

#include <cstdint>

namespace RC::Unreal
{
    class UObject;
    class UFunction;
    class UStruct;
    class FProperty;
}

namespace PalCrosschat
{
    class ChatInject
    {
    public:
        explicit ChatInject(const Config& config);

        // Drain up to MaxBroadcastsPerTick from inbound and broadcast each.
        // Call only from on_update (game thread).
        void Drain(InboundQueue& inbound, int max_per_tick);

    private:
        bool EnsureGameState();
        bool EnsureBroadcastFunction();
        bool BroadcastOne(const InboundMessage& msg);

        std::string PrefixForOrigin(const std::string& origin) const;

        const Config& m_config;
        RC::Unreal::UObject* m_game_state = nullptr;
        RC::Unreal::UFunction* m_broadcast_fn = nullptr;

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
