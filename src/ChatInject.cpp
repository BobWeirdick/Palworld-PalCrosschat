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

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

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
                // v1.88: after a successful ProcessEvent we skip DestroyStruct.
                // Destroying FStrings/TArrays filled via mod code has correlated with
                // delayed PalServer FString heap fatals (log ends mid-BroadcastDual).
                if (m_initialized && m_chat_struct && m_data && !m_leak_after_pe)
                {
                    m_chat_struct->DestroyStruct(m_data.get() + m_chat_offset);
                }
            }

            ParamBufferGuard(const ParamBufferGuard&) = delete;
            ParamBufferGuard& operator=(const ParamBufferGuard&) = delete;

            void LeakAfterProcessEvent() { m_leak_after_pe = true; }

            uint8* Data() { return m_data.get(); }
            uint8* ChatMessage() { return m_data.get() + m_chat_offset; }

        private:
            UFunction* m_fn = nullptr;
            const UStruct* m_chat_struct = nullptr;
            int32_t m_chat_offset = 0;
            size_t m_size = 0;
            std::unique_ptr<uint8[]> m_data;
            bool m_initialized = false;
            bool m_leak_after_pe = false;
        };

        constexpr size_t kMaxDeferredActions = 64;

        // Warm up after GameState+World appear before any BroadcastChatMessage.
        constexpr auto kInjectWarmup = std::chrono::seconds(15);
        constexpr auto kInjectRamp = std::chrono::seconds(60);

        struct RawFString
        {
            TCHAR* data;
            int32 num;
            int32 max;
        };
        static_assert(sizeof(RawFString) == 16, "FString layout assumption");

        StringType Utf8ToUe(const std::string& utf8)
        {
            return RC::ensure_str(utf8);
        }

        void GameFree(void* ptr)
        {
            if (ptr && GMalloc && *GMalloc)
            {
                (*GMalloc)->Free(ptr);
            }
        }

        void* GameMalloc(size_t bytes)
        {
            if (!GMalloc || !*GMalloc || bytes == 0)
            {
                return nullptr;
            }
            return (*GMalloc)->Malloc(bytes, DEFAULT_ALIGNMENT);
        }

        // Fill InitializeStruct'd FString using the game allocator only.
        // Never FString::operator= / AppendChars / Empty (UE4SS TArray paths).
        void FillInitFString(FString* dest, const std::string& utf8)
        {
            if (!dest)
            {
                return;
            }

            auto* raw = reinterpret_cast<RawFString*>(dest);
            if (raw->num < 0 || raw->max < 0 || raw->num > raw->max || raw->max > 0x100000)
            {
                return;
            }

            // Drop any InitializeStruct buffer without UE4SS Empty().
            if (raw->data)
            {
                GameFree(raw->data);
            }
            raw->data = nullptr;
            raw->num = 0;
            raw->max = 0;

            const StringType text = Utf8ToUe(utf8);
            if (text.empty())
            {
                return;
            }

            const int32 len = static_cast<int32>(text.length());
            const int32 max = len + 1;
            auto* buf = static_cast<TCHAR*>(GameMalloc(static_cast<size_t>(max) * sizeof(TCHAR)));
            if (!buf)
            {
                return;
            }
            std::memcpy(buf, text.c_str(), static_cast<size_t>(len) * sizeof(TCHAR));
            buf[len] = static_cast<TCHAR>(0);
            raw->data = buf;
            raw->num = max; // includes null terminator (UE FString convention)
            raw->max = max;
        }

        struct PeCtx
        {
            UObject* obj = nullptr;
            UFunction* fn = nullptr;
            void* params = nullptr;
        };

        int ProcessEventSeh(PeCtx* ctx)
        {
            __try
            {
                ctx->obj->ProcessEvent(ctx->fn, ctx->params);
                return 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 1;
            }
        }

        bool SafeProcessEvent(UObject* obj, UFunction* fn, void* params)
        {
            if (!obj || !fn || !params)
            {
                return false;
            }
            PeCtx ctx{obj, fn, params};
            if (ProcessEventSeh(&ctx) != 0)
            {
                Output::send<LogLevel::Error>(
                    STR("[PalCrosschat] ProcessEvent faulted (BroadcastChatMessage); skipped destroy\n"));
                return false;
            }
            return true;
        }

        bool HasWorldObject()
        {
            try
            {
                return UObjectGlobals::FindFirstOf(STR("World")) != nullptr;
            }
            catch (...)
            {
                return false;
            }
        }
    }

    ChatInject::ChatInject(const Config& config, WordFilter* filter, AudienceTracker* audience)
        : m_config(config), m_filter(filter), m_audience(audience)
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
                                      const std::vector<FGuid>* receivers,
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
            FillInitFString(sender_fs, display_sender);
        }

        {
            // Non-nil SenderPlayerUId: console (Xbox) clients can attribute the line
            // (nil uid → body masked as ***). Dual-broadcast callers pass a uid only on
            // the Xbox plain path (unformatted body); formatted PC path passes nullptr.
            // Unresolvable non-nil uids render as "------" (v1.68); only pass real PlayerUIds.
            FGuid* uid = std::bit_cast<FGuid*>(chat + m_off_sender_uid);
            *uid = sender_player_uid ? *sender_player_uid : FGuid{};
        }

        {
            FString* message_fs = std::bit_cast<FString*>(chat + m_off_message);
            FillInitFString(message_fs, message);
        }

        if (receivers && !receivers->empty())
        {
            auto* receiver_array = std::bit_cast<TArray<FGuid>*>(chat + m_off_receivers);
            for (const FGuid& uid : *receivers)
            {
                receiver_array->Add(uid);
            }
        }

        // MessageArgKeys / MessageArgValues: leave empty (InitializeStruct).
        // MessageId: leave NAME_None (zeroed / default).

        // Re-resolve immediately before ProcessEvent in case GC ran while building params.
        game_state = ResolveGameState();
        if (!game_state)
        {
            return false;
        }

        if (!SafeProcessEvent(game_state, m_broadcast_fn, guard.Data()))
        {
            // Fault during PE: do not DestroyStruct (may already be half-consumed).
            guard.LeakAfterProcessEvent();
            return false;
        }

        // Success: skip DestroyStruct — game copied what it needed; tearing down
        // mod-filled FStrings here has been tied to delayed dedicated-server fatals.
        guard.LeakAfterProcessEvent();
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
        action.receivers = {receiver_player_uid};
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
                if (!action.receivers.empty())
                {
                    BroadcastDisplay(
                        "PalCrosschat", action.message, action.category, &action.receivers);
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
        auto [xbox_sender, xbox_message] = ApplyChatFormatAttributed(
            m_config.chat_format, prefix, clean_guild, clean_sender, clean_message);
        if (xbox_message.empty())
        {
            xbox_message = clean_message;
        }
        const bool have_sender_uid = !(sender_player_uid == FGuid{});
        return BroadcastDual(xbox_sender,
                             xbox_message,
                             display_sender,
                             display_message,
                             category,
                             have_sender_uid ? &sender_player_uid : nullptr);
    }

    bool ChatInject::BroadcastDual(const std::string& xbox_sender,
                                   const std::string& xbox_message,
                                   const std::string& formatted_sender,
                                   const std::string& formatted_message,
                                   uint8_t category,
                                   const FGuid* sender_player_uid)
    {
        // Cache-only — FindAllOf / UniqueNetId run on AudienceTracker::TickRefresh (on_update).
        std::vector<FGuid> xbox;
        std::vector<FGuid> others;
        if (m_audience)
        {
            m_audience->Snapshot(xbox, others);
        }

        // Remote / non-local PlayerUIds render as "------" — only attribute local senders.
        const FGuid* xbox_uid = nullptr;
        if (sender_player_uid && m_audience && m_audience->IsOnlinePlayerUid(*sender_player_uid))
        {
            xbox_uid = sender_player_uid;
        }

        if (m_config.debug_verbose)
        {
            Output::send<LogLevel::Normal>(
                STR("[PalCrosschat] BroadcastDual xbox={} others={} have_uid={}\n"),
                static_cast<int>(xbox.size()),
                static_cast<int>(others.size()),
                xbox_uid ? 1 : 0);
        }

        if (xbox.empty())
        {
            // No known Xbox online — one formatted broadcast to everyone (Steam/PC).
            return BroadcastDisplay(
                formatted_sender, formatted_message, category, nullptr, nullptr);
        }

        // gdk_ present: formatted+nil to everyone else; attributed+local uid to Xbox.
        bool ok = true;
        if (!others.empty())
        {
            ok = BroadcastDisplay(
                     formatted_sender, formatted_message, category, &others, nullptr) &&
                 ok;
        }
        if (xbox_uid)
        {
            ok = BroadcastDisplay(xbox_sender, xbox_message, category, &xbox, xbox_uid) && ok;
        }
        else
        {
            // Cross-server/Discord: nil uid (avoids "------"); Xbox may still mask.
            ok = BroadcastDisplay(xbox_sender, xbox_message, category, &xbox, nullptr) && ok;
        }
        return ok;
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
                FillInitFString(msg, clean);
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
                FillInitFString(msg, clean);
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
        auto [xbox_sender, xbox_message] = ApplyChatFormatAttributed(
            m_config.chat_format, prefix, clean_guild, clean_sender, clean_message);
        if (xbox_message.empty())
        {
            xbox_message = clean_message;
        }

        // Origin server's PlayerUId when present (Discord rows have none): Xbox
        // plain path needs it; formatted PC path uses nil uid.
        FGuid sender_uid{};
        const bool have_sender_uid = TryParseGuid(msg.sender_id, sender_uid);
        return BroadcastDual(xbox_sender,
                             xbox_message,
                             display_sender,
                             display_message,
                             m_config.inject_category,
                             have_sender_uid ? &sender_uid : nullptr);
    }

    bool ChatInject::CanInjectNow(int& out_max_this_tick, int configured_max)
    {
        out_max_this_tick = 0;
        if (!HasWorldObject() || !ResolveGameState())
        {
            return false;
        }

        const auto now = std::chrono::steady_clock::now();
        if (m_world_ready_at.time_since_epoch().count() == 0)
        {
            m_world_ready_at = now;
            if (!m_world_ready_logged)
            {
                m_world_ready_logged = true;
                Output::send<LogLevel::Normal>(
                    STR("[PalCrosschat] World ready; inject warm-up {}s\n"),
                    static_cast<int>(kInjectWarmup.count()));
            }
        }

        if (now - m_world_ready_at < kInjectWarmup)
        {
            return false;
        }

        if (!m_inject_enabled_logged)
        {
            m_inject_enabled_logged = true;
            Output::send<LogLevel::Normal>(
                STR("[PalCrosschat] Inject enabled (warm-up complete)\n"));
        }

        const int cap = configured_max > 0 ? configured_max : 1;
        if (now - m_world_ready_at < kInjectRamp)
        {
            out_max_this_tick = 1; // drain backlog slowly after restart
        }
        else
        {
            out_max_this_tick = cap;
        }
        return true;
    }

    void ChatInject::Drain(InboundQueue& inbound, int max_per_tick)
    {
        try
        {
            int limit = 0;
            if (!CanInjectNow(limit, max_per_tick))
            {
                return;
            }

            // Finish hook-queued ProcessEvent work before inbound injects.
            FlushDeferred(limit > 0 ? limit : 1);

            for (int i = 0; i < limit; ++i)
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
