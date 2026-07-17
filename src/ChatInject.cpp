#include "ChatInject.h"
#include "PalChatApi.h"
#include "Sanitize.h"

#include <bit>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Helpers/String.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealCoreStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FMemory.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace PalCrosschat
{
    namespace
    {
        std::chrono::steady_clock::time_point g_last_inject_exception{};

        class ParamBufferGuard
        {
        public:
            ParamBufferGuard(UFunction* fn, const UStruct* chat_struct, int32_t chat_offset)
                : m_fn(fn), m_chat_struct(chat_struct), m_chat_offset(chat_offset)
            {
                const auto size = fn->GetParmsSize();
                m_size = size;
                m_data = std::make_unique<uint8[]>(size);
                std::memset(m_data.get(), 0, size);
                if (m_chat_struct)
                {
                    m_chat_struct->InitializeStruct(m_data.get() + m_chat_offset);
                    m_initialized = true;
                }
            }

            ~ParamBufferGuard()
            {
                if (m_initialized && m_chat_struct && m_data)
                {
                    m_chat_struct->DestroyStruct(m_data.get() + m_chat_offset);
                }
            }

            ParamBufferGuard(const ParamBufferGuard&) = delete;
            ParamBufferGuard& operator=(const ParamBufferGuard&) = delete;

            uint8* Data() { return m_data.get(); }
            uint8* ChatMessage() { return m_data.get() + m_chat_offset; }

        private:
            UFunction* m_fn = nullptr;
            const UStruct* m_chat_struct = nullptr;
            int32_t m_chat_offset = 0;
            size_t m_size = 0;
            std::unique_ptr<uint8[]> m_data;
            bool m_initialized = false;
        };

        bool ObjectLooksValid(UObject* obj)
        {
            return obj != nullptr;
        }

        StringType Utf8ToUe(const std::string& utf8)
        {
            return RC::ensure_str(utf8);
        }
    }

    ChatInject::ChatInject(const Config& config) : m_config(config) {}

    std::string ChatInject::PrefixForOrigin(const std::string& origin) const
    {
        if (origin == "pal-na")
        {
            return m_config.prefix_na;
        }
        if (origin == "pal-eu")
        {
            return m_config.prefix_eu;
        }
        if (origin == "discord")
        {
            return m_config.prefix_discord;
        }
        return {};
    }

    bool ChatInject::EnsureGameState()
    {
        if (ObjectLooksValid(m_game_state))
        {
            // Cheap staleness check: name still contains expected class fragment.
            try
            {
                const auto name = m_game_state->GetFullName();
                if (name.find(STR("PalGameStateInGame")) != StringType::npos)
                {
                    return true;
                }
            }
            catch (...)
            {
            }
            m_game_state = nullptr;
        }

        m_game_state = UObjectGlobals::FindFirstOf(STR("PalGameStateInGame"));
        return ObjectLooksValid(m_game_state);
    }

    bool ChatInject::EnsureBroadcastFunction()
    {
        if (m_broadcast_fn && m_offsets_ready)
        {
            return true;
        }

        m_broadcast_fn =
            UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, BROADCAST_FUNC_PATH);
        if (!m_broadcast_fn)
        {
            Output::send<LogLevel::Error>(
                STR("[PalCrosschat] Could not find {}. See README \"After a Palworld update\".\n"),
                BROADCAST_FUNC_PATH);
            return false;
        }

        FProperty* chat_prop = m_broadcast_fn->FindProperty(FName(STR("ChatMessage"), FNAME_Find));
        if (!chat_prop)
        {
            Output::send<LogLevel::Error>(
                STR("[PalCrosschat] BroadcastChatMessage missing ChatMessage property\n"));
            return false;
        }

        auto* struct_prop = static_cast<FStructProperty*>(chat_prop);
        m_chat_struct = struct_prop->GetStruct();
        m_chat_param_offset = chat_prop->GetOffset_Internal();
        if (!m_chat_struct)
        {
            Output::send<LogLevel::Error>(
                STR("[PalCrosschat] ChatMessage has no UStruct\n"));
            return false;
        }

        auto resolve = [&](const TCHAR* name) -> int32_t {
            FProperty* p = const_cast<UStruct*>(m_chat_struct)->FindProperty(FName(name, FNAME_Find));
            if (!p)
            {
                Output::send<LogLevel::Error>(
                    STR("[PalCrosschat] FPalChatMessage missing field {}\n"), name);
                return -1;
            }
            return p->GetOffset_Internal();
        };

        m_off_category = resolve(STR("Category"));
        m_off_sender = resolve(STR("Sender"));
        m_off_sender_uid = resolve(STR("SenderPlayerUId"));
        m_off_message = resolve(STR("Message"));
        m_off_receivers = resolve(STR("ReceiverPlayerUIds"));
        m_off_message_id = resolve(STR("MessageId"));
        m_off_arg_keys = resolve(STR("MessageArgKeys"));
        m_off_arg_values = resolve(STR("MessageArgValues"));

        if (m_off_category < 0 || m_off_sender < 0 || m_off_sender_uid < 0 || m_off_message < 0 ||
            m_off_receivers < 0 || m_off_message_id < 0 || m_off_arg_keys < 0 || m_off_arg_values < 0)
        {
            return false;
        }

        m_offsets_ready = true;
        Output::send<LogLevel::Normal>(
            STR("[PalCrosschat] BroadcastChatMessage resolved; ChatMessage offsets ready\n"));
        return true;
    }

    bool ChatInject::BroadcastOne(const InboundMessage& msg)
    {
        if (!EnsureGameState() || !EnsureBroadcastFunction())
        {
            return false;
        }

        std::string clean_message = SanitizeMessage(msg.message);
        if (clean_message.empty())
        {
            return true; // drop silently
        }

        std::string clean_sender = SanitizeMessage(msg.sender_name, 64);
        if (clean_sender.empty())
        {
            clean_sender = "Unknown";
        }

        const std::string prefix = PrefixForOrigin(msg.origin);
        std::string display_sender;
        if (prefix.empty())
        {
            display_sender = clean_sender;
        }
        else
        {
            display_sender = prefix + " " + clean_sender;
        }

        ParamBufferGuard guard(m_broadcast_fn, m_chat_struct, m_chat_param_offset);
        uint8* chat = guard.ChatMessage();

        *std::bit_cast<uint8*>(chat + m_off_category) = m_config.inject_category;

        {
            FString* sender_fs = std::bit_cast<FString*>(chat + m_off_sender);
            *sender_fs = FString(Utf8ToUe(display_sender));
        }

        {
            FGuid* uid = std::bit_cast<FGuid*>(chat + m_off_sender_uid);
            *uid = FGuid{};
        }

        {
            FString* message_fs = std::bit_cast<FString*>(chat + m_off_message);
            *message_fs = FString(Utf8ToUe(clean_message));
        }

        // ReceiverPlayerUIds / MessageArgKeys / MessageArgValues: leave empty (InitializeStruct).
        // MessageId: leave NAME_None (zeroed / default).

        m_game_state->ProcessEvent(m_broadcast_fn, guard.Data());
        return true;
    }

    void ChatInject::Drain(InboundQueue& inbound, int max_per_tick)
    {
        try
        {
            for (int i = 0; i < max_per_tick; ++i)
            {
                // THREAD BOUNDARY: inbound queue -> game thread (plain structs only). No MySQL here.
                auto item = inbound.TryPop();
                if (!item)
                {
                    break;
                }

                if (!BroadcastOne(*item))
                {
                    // Push back so we retry after GameState is ready.
                    inbound.Push(std::move(*item));
                    break;
                }
            }
        }
        catch (const std::exception& ex)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now - g_last_inject_exception > std::chrono::seconds(60))
            {
                g_last_inject_exception = now;
                Output::send<LogLevel::Error>(
                    STR("[PalCrosschat] Exception in inject tick: {}\n"),
                    RC::ensure_str(ex.what()));
            }
        }
        catch (...)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now - g_last_inject_exception > std::chrono::seconds(60))
            {
                g_last_inject_exception = now;
                Output::send<LogLevel::Error>(
                    STR("[PalCrosschat] Unknown exception in inject tick\n"));
            }
        }
    }
}
