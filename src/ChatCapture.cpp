#include "ChatCapture.h"
#include "PalChatApi.h"
#include "Sanitize.h"

#include <chrono>
#include <cstdio>
#include <string>
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

        std::string ReadPlayerUId(UObject* controller, UObject* player_state)
        {
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
                            return FormatGuid(*guid);
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
                        return FormatGuid(*guid);
                    }
                }
            }

            return {};
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
            UFunction* fn = context.TheStack.Node();
            void* locals = context.TheStack.Locals();
            if (!fn || !locals)
            {
                return;
            }
            if (FProperty* msg_prop = fn->FindProperty(FName(STR("Message"), FNAME_Find)))
            {
                if (FString* msg = msg_prop->ContainerPtrToValuePtr<FString>(locals))
                {
                    *msg = FString(STR(""));
                }
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

    ChatCapture::ChatCapture(const Config& config, OutboundQueue& outbound, WebhookWorker* webhook)
        : m_config(config), m_outbound(outbound), m_webhook(webhook)
    {
        m_filter = std::make_unique<WordFilter>(config);
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
        std::string sender_id = ReadPlayerUId(controller, player_state);
        const std::string mute_key = !sender_id.empty() ? sender_id : sender_name;

        if (m_filter && m_filter->Active())
        {
            if (m_filter->IsMuted(mute_key))
            {
                ClearMessageInLocals(context);
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
                m_filter->Mute(mute_key);

                const int minutes = m_filter->AutoMuteMinutes();
                Output::send<LogLevel::Warning>(
                    STR("[PalCrosschat] WordBlacklist hit: player={} id={} mute={}m pattern={} msg={}\n"),
                    RC::ensure_str(sender_name),
                    RC::ensure_str(sender_id.empty() ? std::string("-") : sender_id),
                    minutes,
                    RC::ensure_str(TruncateForLog(*matched, 80)),
                    RC::ensure_str(TruncateForLog(message, 120)));

                if (m_webhook && !m_filter->MuteLogWebhook().empty())
                {
                    std::string content = "**[PalCrosschat] WordBlacklist**\n";
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
                        content += "Action: `blocked` (AutoMuteMinutes=0)\n";
                    }
                    content += "Pattern: `" + TruncateForLog(*matched, 120) + "`\n";
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

        const std::vector<std::string> prefixes = {
            m_config.prefix_na,
            m_config.prefix_eu,
            m_config.prefix_discord,
        };
        if (StartsWithAnyPrefix(message, prefixes))
        {
            return;
        }

        OutboundMessage outbound;
        outbound.sender_name = std::move(sender_name);
        outbound.sender_id = std::move(sender_id);
        outbound.message = std::move(message);

        // THREAD BOUNDARY: game thread -> outbound queue (plain structs only). No MySQL here.
        m_outbound.Push(std::move(outbound));
    }
}
