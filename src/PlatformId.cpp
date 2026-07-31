#include "PlatformId.h"

#include <cctype>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Helpers/String.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealFlags.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace PalCrosschat
{
    namespace
    {
        std::string FStringToUtf8(const FString& str)
        {
            const TCHAR* data = *str;
            if (!data)
            {
                return {};
            }
            return RC::to_utf8_string(data);
        }

        bool IsDigits(const std::string& s)
        {
            return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos;
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
    }

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

    std::string ResolvePlatformUserId(UObject* player_state)
    {
        if (std::string account = NormalizePlatformUserId(ReadAccountName(player_state));
            !account.empty())
        {
            return account;
        }

        std::string via_uid;
        UniqueNetIdSehCtx ctx{};
        ctx.player_state = player_state;
        ctx.out = &via_uid;
        ctx.fn = &UniqueNetIdSehCall;
        if (!UniqueNetIdSehInvoke(&ctx))
        {
            Output::send<LogLevel::Warning>(
                STR("[PalCrosschat] UniqueNetId platform resolve faulted; skipped\n"));
            via_uid.clear();
        }
        return via_uid;
    }
}
