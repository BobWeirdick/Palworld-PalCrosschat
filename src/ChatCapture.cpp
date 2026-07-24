#include "ChatCapture.h"
#include "ChatInject.h"
#include "PalChatApi.h"
#include "Sanitize.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Helpers/String.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealCoreStructs.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/UnrealFlags.hpp>
#include <Unreal/FFrame.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace PalCrosschat
{
    namespace
    {
        std::chrono::steady_clock::time_point g_last_hook_exception{};

        std::string FStringToUtf8(const FString& str)
        {
            const TCHAR* data = *str;
            if (!data)
            {
                return {};
            }
            return RC::to_utf8_string(data);
        }

        std::string FormatGuid(const FGuid& guid)
        {
            char buf[64]{};
            // Unreal FGuid component order A-B-C-D formatted as standard hex groups.
            std::snprintf(buf,
                          sizeof(buf),
                          "%08X-%04X-%04X-%04X-%04X%08X",
                          guid.A,
                          (guid.B >> 16) & 0xFFFF,
                          guid.B & 0xFFFF,
                          (guid.C >> 16) & 0xFFFF,
                          guid.C & 0xFFFF,
                          guid.D);
            return std::string(buf);
        }

        UObject* GetPlayerState(UObject* controller)
        {
            if (!controller)
            {
                return nullptr;
            }

            if (FProperty* prop = controller->GetPropertyByNameInChain(STR("PlayerState")))
            {
                if (UObject** ptr = prop->ContainerPtrToValuePtr<UObject*>(controller))
                {
                    return *ptr;
                }
            }
            return nullptr;
        }

        std::string ReadPlayerName(UObject* player_state)
        {
            if (!player_state)
            {
                return "Unknown";
            }

            if (UFunction* fn = player_state->GetFunctionByNameInChain(STR("GetPlayerName")))
            {
                const auto size = fn->GetParmsSize();
                std::vector<uint8> buf(size > 0 ? size : sizeof(FString), 0);
                player_state->ProcessEvent(fn, buf.data());
                if (FProperty* ret = fn->GetReturnProperty())
                {
                    if (FString* name = ret->ContainerPtrToValuePtr<FString>(buf.data()))
                    {
                        std::string utf8 = FStringToUtf8(*name);
                        if (!utf8.empty())
                        {
                            return utf8;
                        }
                    }
                }
            }

            if (FProperty* prop = player_state->GetPropertyByNameInChain(STR("PlayerName")))
            {
                if (FString* name = prop->ContainerPtrToValuePtr<FString>(player_state))
                {
                    std::string utf8 = FStringToUtf8(*name);
                    if (!utf8.empty())
                    {
                        return utf8;
                    }
                }
            }

            return "Unknown";
        }

        // APalPlayerState::GuildBelongTo -> UPalGroupGuildBase::GetGuildName / GuildName
        std::string ReadGuildName(UObject* player_state)
        {
            if (!player_state)
            {
                return {};
            }

            UObject* guild = nullptr;
            if (FProperty* prop = player_state->GetPropertyByNameInChain(STR("GuildBelongTo")))
            {
                if (UObject** ptr = prop->ContainerPtrToValuePtr<UObject*>(player_state))
                {
                    guild = *ptr;
                }
            }
            if (!guild)
            {
                return {};
            }

            if (UFunction* fn = guild->GetFunctionByNameInChain(STR("GetGuildName")))
            {
                const auto size = fn->GetParmsSize();
                std::vector<uint8> buf(size > 0 ? size : sizeof(FString), 0);
                guild->ProcessEvent(fn, buf.data());
                if (FProperty* ret = fn->GetReturnProperty())
                {
                    if (FString* name = ret->ContainerPtrToValuePtr<FString>(buf.data()))
                    {
                        std::string utf8 = FStringToUtf8(*name);
                        if (!utf8.empty())
                        {
                            return utf8;
                        }
                    }
                }
            }

            if (FProperty* prop = guild->GetPropertyByNameInChain(STR("GuildName")))
            {
                if (FString* name = prop->ContainerPtrToValuePtr<FString>(guild))
                {
                    return FStringToUtf8(*name);
                }
            }

            return {};
        }

        bool IsDigits(const std::string& s)
        {
            return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos;
        }

        // Normalize AccountName / UniqueNetId into steam_/gdk_/ps5_ form.
        std::string NormalizePlatformUserId(std::string raw)
        {
            while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.front())))
            {
                raw.erase(raw.begin());
            }
            while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back())))
            {
                raw.pop_back();
            }
            if (raw.empty())
            {
                return {};
            }

            if (raw.rfind("steam_", 0) == 0 || raw.rfind("gdk_", 0) == 0 || raw.rfind("ps5_", 0) == 0)
            {
                return raw;
            }

            if (raw.rfind("Steam:", 0) == 0 || raw.rfind("steam:", 0) == 0)
            {
                std::string id = raw.substr(6);
                if (IsDigits(id))
                {
                    return "steam_" + id;
                }
            }

            if (IsDigits(raw) && raw.size() >= 15)
            {
                return "steam_" + raw;
            }

            return {};
        }

        std::string ReadAccountName(UObject* player_state)
        {
            if (!player_state)
            {
                return {};
            }
            if (FProperty* prop = player_state->GetPropertyByNameInChain(STR("AccountName")))
            {
                if (FString* name = prop->ContainerPtrToValuePtr<FString>(player_state))
                {
                    return FStringToUtf8(*name);
                }
            }
            return {};
        }

        // BP_GetUniqueId / GetUniqueId -> FUniqueNetIdRepl, then PalUtility::GetPlayerUniqueIdToString.
        // AccountName is often empty on dedicated servers; UniqueNetId is the reliable path.
        std::string ResolveViaUniqueNetId(UObject* player_state)
        {
            if (!player_state)
            {
                return {};
            }

            UFunction* get_uid = player_state->GetFunctionByNameInChain(STR("BP_GetUniqueId"));
            if (!get_uid)
            {
                get_uid = player_state->GetFunctionByNameInChain(STR("GetUniqueId"));
            }
            if (!get_uid)
            {
                return {};
            }

            FProperty* uid_ret = get_uid->GetReturnProperty();
            if (!uid_ret)
            {
                return {};
            }

            const auto uid_parms_size = get_uid->GetParmsSize();
            std::vector<uint8> uid_buf(uid_parms_size > 0 ? uid_parms_size : uid_ret->GetElementSize(), 0);
            player_state->ProcessEvent(get_uid, uid_buf.data());
            void* uid_value = uid_ret->ContainerPtrToValuePtr<void>(uid_buf.data());
            if (!uid_value)
            {
                return {};
            }

            UObject* util = UObjectGlobals::StaticFindObject<UObject*>(
                nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
            if (!util)
            {
                return {};
            }

            UFunction* to_str = util->GetFunctionByNameInChain(STR("GetPlayerUniqueIdToString"));
            if (!to_str)
            {
                return {};
            }

            FProperty* user_id_prop = to_str->FindProperty(FName(STR("UserId"), FNAME_Find));
            FProperty* str_ret = to_str->GetReturnProperty();
            if (!user_id_prop || !str_ret)
            {
                return {};
            }

            const auto to_str_size = to_str->GetParmsSize();
            std::vector<uint8> to_str_buf(to_str_size > 0 ? to_str_size : 64, 0);
            for (FProperty* prop : TFieldRange<FProperty>(to_str, EFieldIterationFlags::IncludeDeprecated))
            {
                if (!prop->HasAnyPropertyFlags(CPF_Parm) || prop->HasAnyPropertyFlags(CPF_ReturnParm))
                {
                    continue;
                }
                prop->InitializeValue(prop->ContainerPtrToValuePtr<void>(to_str_buf.data()));
            }

            user_id_prop->CopyCompleteValue(user_id_prop->ContainerPtrToValuePtr<void>(to_str_buf.data()),
                                            uid_value);
            util->ProcessEvent(to_str, to_str_buf.data());

            std::string result;
            if (FString* fs = str_ret->ContainerPtrToValuePtr<FString>(to_str_buf.data()))
            {
                result = NormalizePlatformUserId(FStringToUtf8(*fs));
            }

            for (FProperty* prop : TFieldRange<FProperty>(to_str, EFieldIterationFlags::IncludeDeprecated))
            {
                if (!prop->HasAnyPropertyFlags(CPF_Parm))
                {
                    continue;
                }
                prop->DestroyValue(prop->ContainerPtrToValuePtr<void>(to_str_buf.data()));
            }

            return result;
        }

        // SEH-safe trampoline: C++ objects must not live in the __try frame.
        struct UniqueNetIdSehCtx
        {
            UObject* player_state = nullptr;
            std::string* out = nullptr;
            void (*fn)(UObject*, std::string*) = nullptr;
        };

        void UniqueNetIdSehCall(UObject* player_state, std::string* out)
        {
            *out = ResolveViaUniqueNetId(player_state);
        }

        int UniqueNetIdSehInvoke(UniqueNetIdSehCtx* ctx)
        {
            __try
            {
                ctx->fn(ctx->player_state, ctx->out);
                return 1;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        std::string ResolvePlatformUserId(UObject* player_state)
        {
            if (std::string account = NormalizePlatformUserId(ReadAccountName(player_state));
                !account.empty())
            {
                return account;
            }

            // UniqueNetId ProcessEvent has crashed dedicated servers; catch AVs.
            std::string via_uid;
            UniqueNetIdSehCtx ctx{};
            ctx.player_state = player_state;
            ctx.out = &via_uid;
            ctx.fn = &UniqueNetIdSehCall;
            if (!UniqueNetIdSehInvoke(&ctx))
            {
                Output::send<LogLevel::Error>(
                    STR("[PalCrosschat] !setdiscord: UniqueNetId resolve access-violated\n"));
                via_uid.clear();
            }
            return via_uid;
        }


        bool TryParseSetDiscord(const std::string& message, std::string& out_code)
        {
            // !setdiscord CODE  (! avoids PalDefender treating /… as admin commands)
            constexpr std::string_view kCmd = "!setdiscord";
            if (message.size() < kCmd.size())
            {
                return false;
            }
            for (size_t i = 0; i < kCmd.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(message[i])) !=
                    static_cast<unsigned char>(kCmd[i]))
                {
                    return false;
                }
            }
            size_t pos = kCmd.size();
            while (pos < message.size() &&
                   std::isspace(static_cast<unsigned char>(message[pos])))
            {
                ++pos;
            }
            if (pos >= message.size())
            {
                return false;
            }
            size_t end = pos;
            while (end < message.size() &&
                   !std::isspace(static_cast<unsigned char>(message[end])))
            {
                ++end;
            }
            out_code = message.substr(pos, end - pos);
            return !out_code.empty();
        }

        bool SplitPlatformUserId(const std::string& platform_user_id,
                                 std::string& out_platform,
                                 std::string& out_user_id)
        {
            const auto underscore = platform_user_id.find('_');
            if (underscore == std::string::npos || underscore == 0 ||
                underscore + 1 >= platform_user_id.size())
            {
                return false;
            }
            out_platform = platform_user_id.substr(0, underscore);
            out_user_id = platform_user_id.substr(underscore + 1);
            return !out_platform.empty() && !out_user_id.empty();
        }

        bool TryReadPlayerUId(UObject* controller, UObject* player_state, FGuid& out_guid)
        {
            out_guid = FGuid{};
            if (controller)
            {
                if (UFunction* fn = controller->GetFunctionByNameInChain(STR("GetPlayerUId")))
                {
                    const auto size = fn->GetParmsSize();
                    std::vector<uint8> buf(size > 0 ? size : sizeof(FGuid), 0);
                    controller->ProcessEvent(fn, buf.data());
                    if (FProperty* ret = fn->GetReturnProperty())
                    {
                        if (FGuid* guid = ret->ContainerPtrToValuePtr<FGuid>(buf.data()))
                        {
                            out_guid = *guid;
                            return out_guid != FGuid{};
                        }
                    }
                }
            }

            if (player_state)
            {
                if (FProperty* prop = player_state->GetPropertyByNameInChain(STR("PlayerUId")))
                {
                    if (FGuid* guid = prop->ContainerPtrToValuePtr<FGuid>(player_state))
                    {
                        out_guid = *guid;
                        return out_guid != FGuid{};
                    }
                }
            }

            return false;
        }

        std::string FormatDuration(int total_seconds)
        {
            if (total_seconds < 0)
            {
                total_seconds = 0;
            }
            const int minutes = total_seconds / 60;
            const int seconds = total_seconds % 60;
            if (minutes <= 0)
            {
                return std::to_string(seconds) + "s";
            }
            if (seconds == 0)
            {
                return std::to_string(minutes) + "m";
            }
            return std::to_string(minutes) + "m " + std::to_string(seconds) + "s";
        }

        std::string ReplacePlaceholder(std::string text,
                                       std::string_view key,
                                       std::string_view value)
        {
            const std::string token = "{" + std::string(key) + "}";
            size_t pos = 0;
            while ((pos = text.find(token, pos)) != std::string::npos)
            {
                text.replace(pos, token.size(), value);
                pos += value.size();
            }
            return text;
        }

        // Banner body. Header "Notifications from the server" is fixed client chrome.
        // Initial: {notification}\n{mutemessage}
        // Active:  {notification} only
        std::string BuildNoticeBanner(std::string_view notification_line,
                                      std::string_view mute_message,
                                      bool include_mute_message)
        {
            std::string body(notification_line);
            if (include_mute_message && !mute_message.empty())
            {
                body.push_back('\n');
                body.append(mute_message);
            }
            return body;
        }

        // EnterChat_Receive(const FString& Message, ...) — Message is CPF_ReferenceParm.
        //
        // Mutation is ONLY safe via OutParms PropAddr. Treating Locals as FString* and
        // assigning into *slot has crashed this host (PalServer+0x326e72a, RSI=UTF-16 text)
        // when Locals actually stores a by-value FString (Data misread as pointer).
        FString* FindMessageViaOutParms(UnrealScriptFunctionCallableContext& context)
        {
            for (FOutParmRec* rec = context.TheStack.OutParms(); rec; rec = rec->NextOutParm)
            {
                if (!rec->Property || !rec->PropAddr)
                {
                    continue;
                }
                if (rec->Property->GetFName() == FName(STR("Message"), FNAME_Find))
                {
                    return reinterpret_cast<FString*>(rec->PropAddr);
                }
            }
            return nullptr;
        }

        bool ReadMessageUtf8(UnrealScriptFunctionCallableContext& context, std::string& out_utf8)
        {
            if (FString* msg = FindMessageViaOutParms(context))
            {
                out_utf8 = FStringToUtf8(*msg);
                return true;
            }

            UFunction* fn = context.TheStack.Node();
            void* locals = context.TheStack.Locals();
            if (!fn || !locals)
            {
                return false;
            }

            FProperty* msg_prop = fn->FindProperty(FName(STR("Message"), FNAME_Find));
            if (!msg_prop)
            {
                return false;
            }

            // Read-only fallbacks. Do not use these pointers for assignment/destroy.
            if (msg_prop->HasAnyPropertyFlags(CPF_ReferenceParm | CPF_OutParm))
            {
                if (FString** slot = msg_prop->ContainerPtrToValuePtr<FString*>(locals))
                {
                    if (FString* pointed = *slot)
                    {
                        out_utf8 = FStringToUtf8(*pointed);
                        return true;
                    }
                }
                return false;
            }

            if (FString* msg = msg_prop->ContainerPtrToValuePtr<FString>(locals))
            {
                out_utf8 = FStringToUtf8(*msg);
                return true;
            }
            return false;
        }

        bool ReadHookParams(UnrealScriptFunctionCallableContext& context,
                            FString& out_message,
                            uint8& out_category)
        {
            out_category = 0;
            out_message = FString(STR(""));

            std::string utf8;
            if (ReadMessageUtf8(context, utf8) && !utf8.empty())
            {
                out_message = FString(RC::ensure_str(utf8).c_str());
            }

            UFunction* fn = context.TheStack.Node();
            void* locals = context.TheStack.Locals();
            if (fn && locals)
            {
                if (FProperty* cat_prop = fn->FindProperty(FName(STR("Category"), FNAME_Find)))
                {
                    if (uint8* cat = cat_prop->ContainerPtrToValuePtr<uint8>(locals))
                    {
                        out_category = *cat;
                    }
                }
            }
            return true;
        }

        // In-place truncation. `*msg = FString("")` frees the game's buffer through the
        // mod's allocator binding and reallocates — that heap mismatch is what caused the
        // delayed PalServer+0x326e72a crashes (freed memory reused, UTF-16 text as pointer).
        // Instead: write L'\0' into the EXISTING buffer and set Num. No alloc, no free,
        // no pointer writes; the game still owns and destroys its own buffer.
        bool TruncateMessageInPlace(FString* msg)
        {
            if (!msg)
            {
                return false;
            }

            struct RawFString
            {
                TCHAR* data;
                int32 num;
                int32 max;
            };
            static_assert(sizeof(RawFString) == 16, "FString layout assumption");
            static_assert(sizeof(FString) == sizeof(RawFString), "FString != {Data,Num,Max}");

            auto* raw = reinterpret_cast<RawFString*>(msg);
            // Sanity: reject garbage layouts before touching anything.
            if (raw->num < 0 || raw->max < 0 || raw->num > raw->max || raw->max > 0x100000)
            {
                return false;
            }
            if (!raw->data || raw->max == 0)
            {
                // Already empty; nothing allocated to touch.
                raw->num = 0;
                return true;
            }

            raw->data[0] = static_cast<TCHAR>(0);
            raw->num = 1; // empty string + terminator, buffer untouched
            return true;
        }

        bool TryClearMessage(UnrealScriptFunctionCallableContext& context)
        {
            if (FString* msg = FindMessageViaOutParms(context))
            {
                return TruncateMessageInPlace(msg);
            }

            // Locals fallback: same FString* we already read the text through.
            UFunction* fn = context.TheStack.Node();
            void* locals = context.TheStack.Locals();
            if (!fn || !locals)
            {
                return false;
            }

            FProperty* msg_prop = fn->FindProperty(FName(STR("Message"), FNAME_Find));
            if (!msg_prop)
            {
                return false;
            }

            if (msg_prop->HasAnyPropertyFlags(CPF_ReferenceParm | CPF_OutParm))
            {
                if (FString** slot = msg_prop->ContainerPtrToValuePtr<FString*>(locals))
                {
                    if (FString* pointed = *slot)
                    {
                        return TruncateMessageInPlace(pointed);
                    }
                }
                return false;
            }

            if (FString* msg = msg_prop->ContainerPtrToValuePtr<FString>(locals))
            {
                return TruncateMessageInPlace(msg);
            }
            return false;
        }

        bool ReadPlayerUIdProperty(UObject* player_state, FGuid& out_guid)
        {
            out_guid = FGuid{};
            if (!player_state)
            {
                return false;
            }
            if (FProperty* prop = player_state->GetPropertyByNameInChain(STR("PlayerUId")))
            {
                if (FGuid* guid = prop->ContainerPtrToValuePtr<FGuid>(player_state))
                {
                    out_guid = *guid;
                    return out_guid != FGuid{};
                }
            }
            return false;
        }

        std::string TruncateForLog(std::string_view input, size_t max_chars)
        {
            if (input.size() <= max_chars)
            {
                return std::string(input);
            }
            return std::string(input.substr(0, max_chars)) + "...";
        }
    }

    ChatCapture::ChatCapture(const Config& config,
                             OutboundQueue& outbound,
                             LinkQueue& link_jobs,
                             WebhookWorker* webhook,
                             WordFilter* filter,
                             ChatInject* inject)
        : m_config(config),
          m_outbound(outbound),
          m_link_jobs(link_jobs),
          m_webhook(webhook),
          m_filter(filter),
          m_inject(inject)
    {
    }

    bool ChatCapture::Register()
    {
        // Pre-hook so WordBlacklist can clear Message before the game broadcasts.
        const auto ids = UObjectGlobals::RegisterHook(
            StringType{CAPTURE_HOOK_PATH},
            &ChatCapture::OnEnterChatReceive,
            UnrealScriptFunctionCallable{},
            this);

        m_hook_ids = ids;
        m_registered = (ids.first != 0 || ids.second != 0);

        UFunction* fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, CAPTURE_HOOK_PATH);
        if (fn && m_registered)
        {
            Output::send<LogLevel::Normal>(
                STR("[PalCrosschat] Capture hook registered on {} (fn={})\n"),
                CAPTURE_HOOK_PATH,
                static_cast<void*>(fn));
            return true;
        }

        Output::send<LogLevel::Error>(
            STR("[PalCrosschat] FAILED to register capture hook on {}. "
                "The game update likely changed this function. "
                "See README section \"After a Palworld update\".\n"),
            CAPTURE_HOOK_PATH);
        return false;
    }

    void ChatCapture::Unregister()
    {
        if (!m_registered)
        {
            return;
        }
        UObjectGlobals::UnregisterHook(StringType{CAPTURE_HOOK_PATH}, m_hook_ids);
        m_registered = false;
    }

    void ChatCapture::OnEnterChatReceive(UnrealScriptFunctionCallableContext& context, void* custom_data)
    {
        auto* self = static_cast<ChatCapture*>(custom_data);
        if (!self)
        {
            return;
        }

        try
        {
            self->HandleHook(context);
        }
        catch (const std::exception& ex)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now - g_last_hook_exception > std::chrono::seconds(60))
            {
                g_last_hook_exception = now;
                Output::send<LogLevel::Error>(
                    STR("[PalCrosschat] Exception in capture hook: {}\n"),
                    RC::ensure_str(ex.what()));
            }
        }
        catch (...)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now - g_last_hook_exception > std::chrono::seconds(60))
            {
                g_last_hook_exception = now;
                Output::send<LogLevel::Error>(
                    STR("[PalCrosschat] Unknown exception in capture hook\n"));
            }
        }
    }

    void ChatCapture::ProcessSetDiscord(UObject* controller, const std::string& connect_code)
    {
        if (!controller)
        {
            return;
        }

        // No GetPlayerName / SendScreenLog ProcessEvent here — those have crashed this server.
        UObject* player_state = GetPlayerState(controller);
        const std::string raw_account = ReadAccountName(player_state);
        std::string sender_name = SanitizeMessage(raw_account, 64);
        if (sender_name.empty())
        {
            sender_name = "Unknown";
        }

        // Prefer property PlayerUId (no ProcessEvent). Cache steam id after first resolve.
        FGuid player_uid{};
        const bool have_uid = ReadPlayerUIdProperty(player_state, player_uid);
        const std::string uid_key = have_uid ? FormatGuid(player_uid) : std::string{};

        std::string platform_user_id;
        if (!uid_key.empty())
        {
            std::lock_guard lock(m_platform_cache_mutex);
            if (auto it = m_platform_by_player_uid.find(uid_key); it != m_platform_by_player_uid.end())
            {
                platform_user_id = it->second;
            }
        }
        if (platform_user_id.empty())
        {
            platform_user_id = ResolvePlatformUserId(player_state);
            if (!platform_user_id.empty() && !uid_key.empty())
            {
                std::lock_guard lock(m_platform_cache_mutex);
                m_platform_by_player_uid[uid_key] = platform_user_id;
            }
        }

        std::string platform;
        std::string user_id;
        if (platform_user_id.empty() ||
            !SplitPlatformUserId(platform_user_id, platform, user_id))
        {
            Output::send<LogLevel::Warning>(
                STR("[PalCrosschat] !setdiscord failed: no platform id (AccountName='{}')\n"),
                RC::ensure_str(raw_account));
            return;
        }

        {
            LinkClient client{};
            client.controller = controller;
            client.player_uid = have_uid ? player_uid : FGuid{};
            std::lock_guard lock(m_link_controllers_mutex);
            m_link_clients[platform_user_id] = client;
        }

        LinkJob job;
        job.connect_code = SanitizeMessage(connect_code, 16);
        job.platform = platform;
        job.user_id = user_id;
        job.platform_user_id = platform_user_id;
        job.player_name = sender_name;
        const int32_t code_len = static_cast<int32_t>(job.connect_code.size());
        m_link_jobs.Push(std::move(job));

        Output::send<LogLevel::Normal>(
            STR("[PalCrosschat] !setdiscord queued for {} (code len={} uid={})\n"),
            RC::ensure_str(platform_user_id),
            code_len,
            have_uid ? 1 : 0);
    }

    void ChatCapture::DeliverLinkResult(const LinkResult& result)
    {
        if (result.notice.empty())
        {
            return;
        }

        Output::send<LogLevel::Normal>(
            STR("[PalCrosschat] {}\n"), RC::ensure_str(result.notice));

        // Chat history feedback (private BroadcastChatMessage). Screen log is easy to miss.
        LinkClient client{};
        bool have_client = false;
        if (!result.platform_user_id.empty())
        {
            std::lock_guard lock(m_link_controllers_mutex);
            if (auto it = m_link_clients.find(result.platform_user_id); it != m_link_clients.end())
            {
                client = it->second;
                m_link_clients.erase(it);
                have_client = true;
            }
        }

        if (!m_inject || !have_client)
        {
            return;
        }

        if (client.player_uid != FGuid{})
        {
            m_inject->EnqueuePrivateChat(client.player_uid, result.notice);
        }
        else if (UObject* controller = client.controller.Get())
        {
            Output::send<LogLevel::Warning>(
                STR("[PalCrosschat] Link feedback: no PlayerUId; falling back to screen log\n"));
            m_inject->EnqueueScreenLog(controller, result.notice);
        }
    }

    void ChatCapture::FlushDeferred()
    {
        for (;;)
        {
            PendingSetDiscord pending;
            {
                std::lock_guard lock(m_pending_mutex);
                if (m_pending_setdiscord.empty())
                {
                    return;
                }
                pending = std::move(m_pending_setdiscord.front());
                m_pending_setdiscord.pop_front();
            }

            if (UObject* controller = pending.controller.Get())
            {
                ProcessSetDiscord(controller, pending.connect_code);
            }
            else
            {
                Output::send<LogLevel::Warning>(
                    STR("[PalCrosschat] !setdiscord: player left before link completed\n"));
            }
        }
    }

    void ChatCapture::HandleHook(UnrealScriptFunctionCallableContext& context)
    {
        FString message_fs;
        uint8 category = 0;
        ReadHookParams(context, message_fs, category);

        std::string raw_message = FStringToUtf8(message_fs);
        std::string message = SanitizeMessage(raw_message);
        if (message.empty())
        {
            return;
        }

        UObject* controller = context.Context;

        // Discord link: suppress chat immediately; resolve UniqueNetId on on_update.
        // Nested ProcessEvent inside EnterChat_Receive crashes the dedicated server.
        {
            std::string connect_code;
            if (TryParseSetDiscord(message, connect_code))
            {
                // Suppress via in-place truncation only (no alloc/free — see
                // TruncateMessageInPlace). FString assignment here corrupted the heap.
                const bool cleared = TryClearMessage(context);
                PendingSetDiscord pending{};
                pending.controller = controller;
                pending.connect_code = SanitizeMessage(connect_code, 16);
                const int32_t code_len = static_cast<int32_t>(pending.connect_code.size());
                {
                    std::lock_guard lock(m_pending_mutex);
                    if (m_pending_setdiscord.size() >= 32)
                    {
                        m_pending_setdiscord.pop_front();
                    }
                    m_pending_setdiscord.push_back(std::move(pending));
                }
                Output::send<LogLevel::Normal>(
                    STR("[PalCrosschat] !setdiscord accepted (cleared={} code len={})\n"),
                    cleared ? 1 : 0,
                    code_len);
                return;
            }
        }

        UObject* player_state = GetPlayerState(controller);
        std::string sender_name = SanitizeMessage(ReadPlayerName(player_state), 64);
        if (sender_name.empty())
        {
            sender_name = "Unknown";
        }
        FGuid sender_uid{};
        const std::string sender_id =
            TryReadPlayerUId(controller, player_state, sender_uid) ? FormatGuid(sender_uid)
                                                                   : std::string{};
        const std::string mute_key = !sender_id.empty() ? sender_id : sender_name;

        auto show_mute_banner = [&](const std::string& banner) {
            if (!m_inject)
            {
                return;
            }
            // Defer ProcessEvent until on_update — nested PE inside EnterChat_Receive is unsafe.
            m_inject->EnqueueServerNotice(banner);
            m_inject->EnqueueScreenLog(controller, banner);
        };

        if (m_filter && m_filter->Active())
        {
            if (m_filter->IsMuted(mute_key))
            {
                if (!TryClearMessage(context) && m_config.debug_verbose)
                {
                    Output::send<LogLevel::Warning>(
                        STR("[PalCrosschat] Mute suppress skipped (no OutParms Message)\n"));
                }

                // Active mute: ActiveMuteNotification only (no MuteMessage). Throttle 15s.
                if (m_filter->TryConsumeMuteNotify(mute_key, std::chrono::seconds(15)))
                {
                    std::string remaining = "?";
                    if (auto secs = m_filter->MuteRemainingSeconds(mute_key))
                    {
                        remaining = FormatDuration(*secs);
                    }
                    const std::string line = ReplacePlaceholder(
                        m_filter->ActiveMuteNotification(), "remainingtime", remaining);
                    show_mute_banner(BuildNoticeBanner(line, {}, false));
                }

                if (m_config.debug_verbose)
                {
                    Output::send<LogLevel::Normal>(
                        STR("[PalCrosschat] Dropped chat from muted player {}\n"),
                        RC::ensure_str(sender_name));
                }
                return;
            }

            if (auto matched = m_filter->FindMatch(message))
            {
                if (!TryClearMessage(context) && m_config.debug_verbose)
                {
                    Output::send<LogLevel::Warning>(
                        STR("[PalCrosschat] Filter suppress skipped (no OutParms Message)\n"));
                }

                const int minutes = matched->mute_minutes;
                const std::string mute_message = matched->mute_message;

                if (minutes > 0)
                {
                    m_filter->Mute(mute_key, mute_message, minutes);
                }

                // Initial mute: InitialMuteNotification + pattern MuteMessage.
                const std::string mutetime = FormatDuration(minutes * 60);
                std::string line = ReplacePlaceholder(
                    m_filter->InitialMuteNotification(), "mutetime", mutetime);
                line = ReplacePlaceholder(line, "mutemessage", mute_message);
                line = ReplacePlaceholder(line, "remainingtime", mutetime);
                show_mute_banner(BuildNoticeBanner(line, mute_message, true));

                Output::send<LogLevel::Warning>(
                    STR("[PalCrosschat] ChatFilter hit: player={} id={} mute={}m pattern={} msg={}\n"),
                    RC::ensure_str(sender_name),
                    RC::ensure_str(sender_id.empty() ? std::string("-") : sender_id),
                    minutes,
                    RC::ensure_str(TruncateForLog(matched->pattern_source, 80)),
                    RC::ensure_str(TruncateForLog(message, 120)));

                if (m_webhook && !m_filter->MuteLogWebhook().empty())
                {
                    std::string content = "**[PalCrosschat] ChatFilter**\n";
                    content += "Server: `" + m_config.server_origin + "`\n";
                    content += "Player: `" + sender_name + "`";
                    if (!sender_id.empty())
                    {
                        content += " (`" + sender_id + "`)";
                    }
                    content += "\n";
                    if (minutes > 0)
                    {
                        content += "Muted: `" + std::to_string(minutes) + "m`\n";
                    }
                    else
                    {
                        content += "Action: `blocked` (MuteMinutes=0)\n";
                    }
                    if (!mute_message.empty())
                    {
                        content += "MuteMessage: `" + TruncateForLog(mute_message, 120) + "`\n";
                    }
                    content += "Pattern: `" + TruncateForLog(matched->pattern_source, 120) + "`\n";
                    content += "Message: `" + TruncateForLog(message, 200) + "`";
                    m_webhook->Enqueue(m_filter->MuteLogWebhook(), std::move(content));
                }
                return;
            }
        }

        // Chat channel trace: confirms which EPalChatCategory the server actually sees
        // per line, so a "guild chat reached Discord" report can be traced to either a
        // misread category byte or a different mod's relay.
        if (m_config.debug_verbose)
        {
            Output::send<LogLevel::Normal>(
                STR("[PalCrosschat] CHAT pal_cat={} db_cat={} relayed={} sender={} msg={}\n"),
                static_cast<int>(category),
                static_cast<int>(ToDbChatCategory(category)),
                IsRelayedCategory(category) ? 1 : 0,
                RC::ensure_str(sender_name),
                RC::ensure_str(TruncateForLog(message, 80)));
        }

        if (!IsRelayedCategory(category))
        {
            return;
        }

        // Loop guard: ignore chat that already looks like a tagged crosschat line.
        std::vector<std::string> prefixes = {
            m_config.prefix_na,
            m_config.prefix_eu,
            m_config.prefix_discord,
        };
        for (const auto& p : {m_config.prefix_na, m_config.prefix_eu, m_config.prefix_discord})
        {
            if (!p.empty() && p.front() != '[')
            {
                prefixes.push_back("[" + p + "]");
            }
        }
        if (StartsWithAnyPrefix(message, prefixes))
        {
            return;
        }

        const std::string guild_name = SanitizeMessage(ReadGuildName(player_state), 64);

        // Rebroadcast local Global chat with ChatFormat when ShowLocalServerTag is on.
        // Queue only — ProcessEvent runs from on_update after this hook returns.
        // Do NOT hook BroadcastChatMessage — join/system chat uses const FPalChatMessage& and
        // mutating Locals there crashes the dedicated server.
        if (m_config.show_local_server_tag && m_inject)
        {
            // sender_uid keeps the rebroadcast attributable to the real player; console
            // clients mask the body of chat whose SenderPlayerUId is empty.
            m_inject->EnqueueLocalTagged(sender_name, guild_name, message, category, sender_uid);
            if (!TryClearMessage(context) && m_config.debug_verbose)
            {
                Output::send<LogLevel::Warning>(
                    STR("[PalCrosschat] Local-tag suppress skipped (no OutParms Message)\n"));
            }
        }

        OutboundMessage outbound;
        outbound.sender_name = std::move(sender_name);
        outbound.sender_id = std::move(sender_id);
        outbound.guild_name = guild_name;
        outbound.message = std::move(message);
        outbound.category = ToDbChatCategory(category);

        // THREAD BOUNDARY: game thread -> outbound queue (plain structs only). No MySQL here.
        m_outbound.Push(std::move(outbound));
    }
}
