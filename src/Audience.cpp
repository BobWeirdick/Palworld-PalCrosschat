#include "Audience.h"

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include <Helpers/String.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>

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

        std::string FormatGuid(const FGuid& guid)
        {
            char buf[64]{};
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
            if (raw.rfind("steam_", 0) == 0 || raw.rfind("gdk_", 0) == 0 ||
                raw.rfind("ps5_", 0) == 0)
            {
                return raw;
            }
            // Bare SteamID64 digits → steam_
            bool digits = !raw.empty();
            for (char c : raw)
            {
                if (c < '0' || c > '9')
                {
                    digits = false;
                    break;
                }
            }
            if (digits && raw.size() >= 15)
            {
                return "steam_" + raw;
            }
            return raw;
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

        bool ReadPlayerUId(UObject* player_state, FGuid& out)
        {
            out = FGuid{};
            if (!player_state)
            {
                return false;
            }
            if (FProperty* prop = player_state->GetPropertyByNameInChain(STR("PlayerUId")))
            {
                if (FGuid* guid = prop->ContainerPtrToValuePtr<FGuid>(player_state))
                {
                    out = *guid;
                    return !(out == FGuid{});
                }
            }
            return false;
        }

        bool IsKnownNonXboxPlatform(const std::string& platform_user_id)
        {
            return platform_user_id.rfind("steam_", 0) == 0 ||
                   platform_user_id.rfind("ps5_", 0) == 0;
        }
    }

    void AudienceTracker::Remember(const FGuid& player_uid, const std::string& platform_user_id)
    {
        if (player_uid == FGuid{} || platform_user_id.empty())
        {
            return;
        }
        const std::string normalized = NormalizePlatformUserId(platform_user_id);
        if (normalized.empty())
        {
            return;
        }
        std::lock_guard lock(m_mutex);
        m_platform_by_uid[FormatGuid(player_uid)] = normalized;
    }

    void AudienceTracker::Collect(std::vector<FGuid>& xbox, std::vector<FGuid>& others)
    {
        xbox.clear();
        others.clear();

        std::vector<UObject*> states;
        UObjectGlobals::FindAllOf(STR("PalPlayerState"), states);

        for (UObject* state : states)
        {
            if (!state)
            {
                continue;
            }

            FGuid uid{};
            if (!ReadPlayerUId(state, uid))
            {
                continue;
            }

            std::string platform;
            const std::string key = FormatGuid(uid);
            {
                std::lock_guard lock(m_mutex);
                if (auto it = m_platform_by_uid.find(key); it != m_platform_by_uid.end())
                {
                    platform = it->second;
                }
            }

            if (platform.empty())
            {
                platform = NormalizePlatformUserId(ReadAccountName(state));
                if (!platform.empty())
                {
                    std::lock_guard lock(m_mutex);
                    m_platform_by_uid[key] = platform;
                }
            }

            // Unknown/empty → xbox bucket. Dedicated often leaves AccountName empty for
            // Xbox; putting unknowns on the formatted nil-UID path censors their chat.
            if (IsKnownNonXboxPlatform(platform))
            {
                others.push_back(uid);
            }
            else
            {
                xbox.push_back(uid);
            }
        }
    }
}
