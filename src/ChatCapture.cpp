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

#include <DynamicOutput/DynamicOutput.hpp>
#include <Helpers/String.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealCoreStructs.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UFunctionStructs.hpp>

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

        // Locals layout for EnterChat_Receive(const FString& Message, uint8 Category).
        // Prefer property-offset reads when available; this mirror is the fallback.
        struct EnterChatReceiveParams
        {
            FString Message;
            uint8 Category;
        };

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

        bool TryParseSetDiscord(const std::string& message, std::string& out_code)
        {
            // /setdiscord CODE  (case-insensitive command)
            constexpr std::string_view kCmd = "/setdiscord";
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

        bool ReadHookParams(UnrealScriptFunctionCallableContext& context,
                            FString& out_message,
                            uint8& out_category)
        {
            // Prefer reflected property offsets on the executing UFunction.
            UFunction* fn = context.TheStack.Node();
            if (fn)
            {
                FProperty* msg_prop = fn->FindProperty(FName(STR("Message"), FNAME_Find));
                FProperty* cat_prop = fn->FindProperty(FName(STR("Category"), FNAME_Find));
                void* locals = context.TheStack.Locals();
                if (msg_prop && cat_prop && locals)
                {
                    if (FString* msg = msg_prop->ContainerPtrToValuePtr<FString>(locals))
                    {
                        out_message = *msg;
                    }
                    if (uint8* cat = cat_prop->ContainerPtrToValuePtr<uint8>(locals))
                    {
                        out_category = *cat;
                    }
                    return true;
                }
            }

            auto& params = context.GetParams<EnterChatReceiveParams>();
            out_message = params.Message;
            out_category = params.Category;
            return true;
        }

        void ClearMessageInLocals(UnrealScriptFunctionCallableContext& context)
        {
            // Suppress broadcast by emptying Message before EnterChat_Receive runs.
            // Use both reflected Locals and GetParams — EnterChat_Receive takes const FString&.
            UFunction* fn = context.TheStack.Node();
            void* locals = context.TheStack.Locals();
            if (fn && locals)
            {
                if (FProperty* msg_prop = fn->FindProperty(FName(STR("Message"), FNAME_Find)))
                {
                    if (FString* msg = msg_prop->ContainerPtrToValuePtr<FString>(locals))
                    {
                        *msg = FString(STR(""));
                    }
                }
            }

            try
            {
                auto& params = context.GetParams<EnterChatReceiveParams>();
                params.Message = FString(STR(""));
            }
            catch (...)
            {
            }
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

        // Discord account link: /setdiscord CODE (never relayed to crosschat).
        {
            std::string connect_code;
            if (TryParseSetDiscord(message, connect_code))
            {
                ClearMessageInLocals(context);

                const std::string platform_user_id =
                    NormalizePlatformUserId(ReadAccountName(player_state));
                std::string platform;
                std::string user_id;
                if (platform_user_id.empty() ||
                    !SplitPlatformUserId(platform_user_id, platform, user_id))
                {
                    show_mute_banner("Discord link failed: could not read platform account id.");
                    return;
                }

                LinkJob job;
                job.connect_code = SanitizeMessage(connect_code, 16);
                job.platform = platform;
                job.user_id = user_id;
                job.platform_user_id = platform_user_id;
                job.player_name = sender_name;
                m_link_jobs.Push(std::move(job));

                if (m_config.debug_verbose)
                {
                    Output::send<LogLevel::Normal>(
                        STR("[PalCrosschat] Queued /setdiscord for {}\n"),
                        RC::ensure_str(platform_user_id));
                }
                return;
            }
        }

        if (m_filter && m_filter->Active())
        {
            if (m_filter->IsMuted(mute_key))
            {
                ClearMessageInLocals(context);

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
                ClearMessageInLocals(context);

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
            m_inject->EnqueueLocalTagged(sender_name, guild_name, message, category);
            ClearMessageInLocals(context);
        }

        OutboundMessage outbound;
        outbound.sender_name = std::move(sender_name);
        outbound.sender_id = std::move(sender_id);
        outbound.guild_name = guild_name;
        outbound.message = std::move(message);

        // THREAD BOUNDARY: game thread -> outbound queue (plain structs only). No MySQL here.
        m_outbound.Push(std::move(outbound));
    }
}
