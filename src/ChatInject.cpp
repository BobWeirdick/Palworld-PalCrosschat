#include "ChatInject.h"
#include "ChatFormat.h"
#include "PalChatApi.h"
#include "Sanitize.h"
#include "WordFilter.h"

#include <bit>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Helpers/String.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealCoreStructs.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Core/Containers/Array.hpp>
#include <Unreal/FMemory.hpp>
#include <Unreal/UnrealFlags.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace PalCrosschat
{
    namespace
    {
        std::chrono::steady_clock::time_point g_last_inject_exception{};

        // Inverse of ChatCapture's FormatGuid: the 32 hex digits are A, B, C, D in order.
        bool TryParseGuid(std::string_view text, FGuid& out)
        {
            uint32_t words[4]{};
            int digits = 0;
            for (const char c : text)
            {
                if (c == '-' || c == '{' || c == '}')
                {
                    continue;
                }

                uint32_t value = 0;
                if (c >= '0' && c <= '9')
                {
                    value = static_cast<uint32_t>(c - '0');
                }
                else if (c >= 'a' && c <= 'f')
                {
                    value = static_cast<uint32_t>(c - 'a' + 10);
                }
                else if (c >= 'A' && c <= 'F')
                {
                    value = static_cast<uint32_t>(c - 'A' + 10);
                }
                else
                {
                    return false;
                }

                if (digits >= 32)
                {
                    return false;
                }
                words[digits / 8] = (words[digits / 8] << 4) | value;
                ++digits;
            }

            if (digits != 32)
            {
                return false;
            }
            out.A = words[0];
            out.B = words[1];
            out.C = words[2];
            out.D = words[3];
            return !(out == FGuid{});
        }

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

        constexpr size_t kMaxDeferredActions = 64;

        StringType Utf8ToUe(const std::string& utf8)
        {
            return RC::ensure_str(utf8);
        }
    }

    ChatInject::ChatInject(const Config& config, WordFilter* filter)
        : m_config(config), m_filter(filter)
    {
    }

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

    std::string ChatInject::LocalServerPrefix() const
    {
        return PrefixForOrigin(m_config.server_origin);
    }

    UObject* ChatInject::ResolveGameState()
    {
        // FWeakObjectPtr survives GameState teardown; raw cache + GetFullName() AV'd in UE4SS.
        if (UObject* cached = m_game_state.Get())
        {
            return cached;
        }

        m_game_state.Reset();
        UObject* found = UObjectGlobals::FindFirstOf(STR("PalGameStateInGame"));
        if (!found)
        {
            return nullptr;
        }

        m_game_state = found;
        return m_game_state.Get();
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

    bool ChatInject::BroadcastDisplay(const std::string& display_sender,
                                      const std::string& message,
                                      uint8_t category,
                                      const FGuid* receiver_only,
                                      const FGuid* sender_player_uid)
    {
        UObject* game_state = ResolveGameState();
        if (!game_state || !EnsureBroadcastFunction())
        {
            return false;
        }

        ParamBufferGuard guard(m_broadcast_fn, m_chat_struct, m_chat_param_offset);
        uint8* chat = guard.ChatMessage();

        *std::bit_cast<uint8*>(chat + m_off_category) = category;

        {
            FString* sender_fs = std::bit_cast<FString*>(chat + m_off_sender);
            *sender_fs = FString(Utf8ToUe(display_sender));
        }

        {
            // Console (Xbox) clients run received chat through Palworld's word-filter
            // service and mask the body when the sender cannot be identified, so an
            // empty SenderPlayerUId shows every injected line as "***".
            FGuid* uid = std::bit_cast<FGuid*>(chat + m_off_sender_uid);
            *uid = (sender_player_uid && m_config.preserve_sender_uid) ? *sender_player_uid
                                                                      : FGuid{};
        }

        {
            FString* message_fs = std::bit_cast<FString*>(chat + m_off_message);
            *message_fs = FString(Utf8ToUe(message));
        }

        if (receiver_only)
        {
            auto* receivers = std::bit_cast<TArray<FGuid>*>(chat + m_off_receivers);
            receivers->Add(*receiver_only);
        }

        // MessageArgKeys / MessageArgValues: leave empty (InitializeStruct).
        // MessageId: leave NAME_None (zeroed / default).

        // Re-resolve immediately before ProcessEvent in case GC ran while building params.
        game_state = ResolveGameState();
        if (!game_state)
        {
            return false;
        }
        game_state->ProcessEvent(m_broadcast_fn, guard.Data());
        return true;
    }

    void ChatInject::EnqueueLocalTagged(const std::string& sender_name,
                                        const std::string& guild_name,
                                        const std::string& message,
                                        uint8_t category,
                                        const FGuid& sender_player_uid)
    {
        DeferredAction action{};
        action.kind = DeferredKind::LocalTagged;
        action.sender_name = sender_name;
        action.guild_name = guild_name;
        action.message = message;
        action.category = category;
        action.sender_uid = sender_player_uid;

        std::lock_guard lock(m_deferred_mutex);
        if (m_deferred.size() >= kMaxDeferredActions)
        {
            m_deferred.pop_front();
        }
        m_deferred.push_back(std::move(action));
    }

    void ChatInject::EnqueueServerNotice(const std::string& notice_message)
    {
        DeferredAction action{};
        action.kind = DeferredKind::ServerNotice;
        action.message = notice_message;

        std::lock_guard lock(m_deferred_mutex);
        if (m_deferred.size() >= kMaxDeferredActions)
        {
            m_deferred.pop_front();
        }
        m_deferred.push_back(std::move(action));
    }

    void ChatInject::EnqueueScreenLog(UObject* player_controller, const std::string& message)
    {
        if (!player_controller)
        {
            return;
        }

        DeferredAction action{};
        action.kind = DeferredKind::ScreenLog;
        action.message = message;
        action.controller = player_controller;

        std::lock_guard lock(m_deferred_mutex);
        if (m_deferred.size() >= kMaxDeferredActions)
        {
            m_deferred.pop_front();
        }
        m_deferred.push_back(std::move(action));
    }

    void ChatInject::EnqueuePrivateChat(const FGuid& receiver_player_uid, const std::string& message)
    {
        if (receiver_player_uid == FGuid{} || message.empty())
        {
            return;
        }

        DeferredAction action{};
        action.kind = DeferredKind::PrivateChat;
        action.message = message;
        action.receiver_uid = receiver_player_uid;
        action.has_receiver = true;
        action.category = CHAT_CATEGORY_GLOBAL;

        std::lock_guard lock(m_deferred_mutex);
        if (m_deferred.size() >= kMaxDeferredActions)
        {
            m_deferred.pop_front();
        }
        m_deferred.push_back(std::move(action));
    }

    void ChatInject::FlushDeferred(int max_actions)
    {
        for (int i = 0; i < max_actions; ++i)
        {
            DeferredAction action;
            {
                std::lock_guard lock(m_deferred_mutex);
                if (m_deferred.empty())
                {
                    return;
                }
                action = std::move(m_deferred.front());
                m_deferred.pop_front();
            }

            switch (action.kind)
            {
            case DeferredKind::LocalTagged:
                BroadcastLocalTagged(action.sender_name,
                                     action.guild_name,
                                     action.message,
                                     action.category,
                                     action.sender_uid);
                break;
            case DeferredKind::ServerNotice:
                ShowServerNotice(action.message);
                break;
            case DeferredKind::ScreenLog:
                if (UObject* controller = action.controller.Get())
                {
                    SendScreenLog(controller, action.message);
                }
                break;
            case DeferredKind::PrivateChat:
                if (action.has_receiver)
                {
                    BroadcastDisplay(
                        "PalCrosschat", action.message, action.category, &action.receiver_uid);
                }
                break;
            }
        }
    }

    bool ChatInject::BroadcastLocalTagged(const std::string& sender_name,
                                          const std::string& guild_name,
                                          const std::string& message,
                                          uint8_t category,
                                          const FGuid& sender_player_uid)
    {
        const std::string prefix = LocalServerPrefix();

        std::string clean_sender = SanitizeMessage(sender_name, 64);
        if (clean_sender.empty())
        {
            clean_sender = "Unknown";
        }

        std::string clean_message = SanitizeMessage(message);
        if (clean_message.empty())
        {
            return false;
        }

        const std::string clean_guild = SanitizeMessage(guild_name, 64);
        auto [display_sender, display_message] = ApplyChatFormat(
            m_config.chat_format, prefix, clean_guild, clean_sender, clean_message);
        // Never clear/replace local chat with an empty Message — client drops it.
        if (display_message.empty())
        {
            return false;
        }
        const bool have_sender_uid = !(sender_player_uid == FGuid{});
        return BroadcastDisplay(display_sender,
                                display_message,
                                category,
                                nullptr,
                                have_sender_uid ? &sender_player_uid : nullptr);
    }

    bool ChatInject::EnsureServerNoticeFunction()
    {
        if (m_server_notice_fn)
        {
            return true;
        }

        m_server_notice_fn =
            UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, SERVER_NOTICE_FUNC_PATH);
        if (!m_server_notice_fn)
        {
            Output::send<LogLevel::Error>(
                STR("[PalCrosschat] Could not find {}\n"), SERVER_NOTICE_FUNC_PATH);
            return false;
        }
        return true;
    }

    bool ChatInject::ShowServerNotice(const std::string& notice_message)
    {
        std::string clean = SanitizeNotice(notice_message, 512);
        if (clean.empty())
        {
            return false;
        }
        UObject* game_state = ResolveGameState();
        if (!game_state || !EnsureServerNoticeFunction())
        {
            return false;
        }

        const auto size = m_server_notice_fn->GetParmsSize();
        std::vector<uint8> buf(size > 0 ? size : sizeof(FString), 0);

        for (FProperty* prop :
             TFieldRange<FProperty>(m_server_notice_fn, EFieldIterationFlags::IncludeDeprecated))
        {
            if (!prop->HasAnyPropertyFlags(CPF_Parm) || prop->HasAnyPropertyFlags(CPF_ReturnParm))
            {
                continue;
            }
            prop->InitializeValue(prop->ContainerPtrToValuePtr<void>(buf.data()));
        }

        if (FProperty* msg_prop =
                m_server_notice_fn->FindProperty(FName(STR("NoticeMessage"), FNAME_Find)))
        {
            if (FString* msg = msg_prop->ContainerPtrToValuePtr<FString>(buf.data()))
            {
                *msg = FString(Utf8ToUe(clean));
            }
        }

        game_state = ResolveGameState();
        if (game_state)
        {
            game_state->ProcessEvent(m_server_notice_fn, buf.data());
        }

        for (FProperty* prop :
             TFieldRange<FProperty>(m_server_notice_fn, EFieldIterationFlags::IncludeDeprecated))
        {
            if (!prop->HasAnyPropertyFlags(CPF_Parm))
            {
                continue;
            }
            prop->DestroyValue(prop->ContainerPtrToValuePtr<void>(buf.data()));
        }
        return game_state != nullptr;
    }

    bool ChatInject::SendScreenLog(UObject* player_controller, const std::string& message)
    {
        // Caller must pass a live object (e.g. from FWeakObjectPtr::Get()).
        if (!player_controller)
        {
            return false;
        }

        std::string clean = SanitizeNotice(message, 512);
        if (clean.empty())
        {
            return false;
        }

        UFunction* fn = player_controller->GetFunctionByNameInChain(STR("SendScreenLogToClient"));
        if (!fn)
        {
            return false;
        }

        const auto size = fn->GetParmsSize();
        std::vector<uint8> buf(size > 0 ? size : 64, 0);

        for (FProperty* prop : TFieldRange<FProperty>(fn, EFieldIterationFlags::IncludeDeprecated))
        {
            if (!prop->HasAnyPropertyFlags(CPF_Parm) || prop->HasAnyPropertyFlags(CPF_ReturnParm))
            {
                continue;
            }
            prop->InitializeValue(prop->ContainerPtrToValuePtr<void>(buf.data()));
        }

        // Best-effort reflected fill: Message, Color (skip / leave default), Duration, Key.
        if (FProperty* msg_prop = fn->FindProperty(FName(STR("Message"), FNAME_Find)))
        {
            if (FString* msg = msg_prop->ContainerPtrToValuePtr<FString>(buf.data()))
            {
                *msg = FString(Utf8ToUe(clean));
            }
        }
        if (FProperty* dur_prop = fn->FindProperty(FName(STR("Duration"), FNAME_Find)))
        {
            if (float* dur = dur_prop->ContainerPtrToValuePtr<float>(buf.data()))
            {
                *dur = 8.0f;
            }
        }
        if (FProperty* key_prop = fn->FindProperty(FName(STR("Key"), FNAME_Find)))
        {
            if (FName* key = key_prop->ContainerPtrToValuePtr<FName>(buf.data()))
            {
                *key = FName(STR("PalCrosschatLink"), FNAME_Add);
            }
        }

        player_controller->ProcessEvent(fn, buf.data());

        for (FProperty* prop : TFieldRange<FProperty>(fn, EFieldIterationFlags::IncludeDeprecated))
        {
            if (!prop->HasAnyPropertyFlags(CPF_Parm))
            {
                continue;
            }
            prop->DestroyValue(prop->ContainerPtrToValuePtr<void>(buf.data()));
        }
        return true;
    }

    bool ChatInject::BroadcastOne(const InboundMessage& msg)
    {
        std::string clean_message = SanitizeMessage(msg.message);
        if (clean_message.empty())
        {
            return true; // drop silently
        }

        // Inbound path never hits EnterChat_Receive — filter cross-server/Discord here too.
        if (m_filter && m_filter->Active())
        {
            if (auto matched = m_filter->FindMatch(clean_message))
            {
                Output::send<LogLevel::Warning>(
                    STR("[PalCrosschat] ChatFilter dropped inbound: origin={} sender={} pattern={} msg={}\n"),
                    RC::ensure_str(msg.origin),
                    RC::ensure_str(msg.sender_name),
                    RC::ensure_str(matched->pattern_source),
                    RC::ensure_str(clean_message));
                return true; // consumed; do not requeue
            }
        }

        std::string clean_sender = SanitizeMessage(msg.sender_name, 64);
        if (clean_sender.empty())
        {
            clean_sender = "Unknown";
        }

        const std::string prefix = PrefixForOrigin(msg.origin);
        const std::string clean_guild = SanitizeMessage(msg.guild_name, 64);
        auto [display_sender, display_message] = ApplyChatFormat(
            m_config.chat_format, prefix, clean_guild, clean_sender, clean_message);
        if (display_message.empty())
        {
            display_message = clean_message;
        }

        // Carry the origin server's PlayerUId when we have one (Discord rows have none):
        // console clients mask chat bodies they cannot attribute to a sender.
        FGuid sender_uid{};
        const bool have_sender_uid = TryParseGuid(msg.sender_id, sender_uid);
        return BroadcastDisplay(display_sender,
                                display_message,
                                m_config.inject_category,
                                nullptr,
                                have_sender_uid ? &sender_uid : nullptr);
    }

    void ChatInject::Drain(InboundQueue& inbound, int max_per_tick)
    {
        try
        {
            // Finish hook-queued ProcessEvent work before inbound injects.
            FlushDeferred(max_per_tick > 0 ? max_per_tick : 8);

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
