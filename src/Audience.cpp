#include "Audience.h"
#include "PlatformId.h"

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

        std::string ReadAccountNameProp(UObject* player_state)
        {
            if (!player_state)
            {
                return {};
            }
            if (FProperty* prop = player_state->GetPropertyByNameInChain(STR("AccountName")))
            {
                if (FString* name = prop->ContainerPtrToValuePtr<FString>(player_state))
                {
                    const TCHAR* data = **name;
                    if (data)
                    {
                        return RC::to_utf8_string(data);
                    }
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

        bool IsXboxPlatform(const std::string& platform_user_id)
        {
            return platform_user_id.rfind("gdk_", 0) == 0;
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

    void AudienceTracker::WarmUnknownPlatforms(int max_resolves)
    {
        if (max_resolves <= 0)
        {
            return;
        }

        std::vector<UObject*> states;
        try
        {
            UObjectGlobals::FindAllOf(STR("PalPlayerState"), states);
        }
        catch (...)
        {
            return;
        }

        int resolved = 0;
        for (UObject* state : states)
        {
            if (resolved >= max_resolves || !state)
            {
                break;
            }

            FGuid uid{};
            if (!ReadPlayerUId(state, uid))
            {
                continue;
            }

            const std::string key = FormatGuid(uid);
            {
                std::lock_guard lock(m_mutex);
                if (m_platform_by_uid.contains(key))
                {
                    continue;
                }
            }

            const std::string platform = ResolvePlatformUserId(state);
            if (platform.empty())
            {
                continue;
            }
            Remember(uid, platform);
            ++resolved;
        }
    }

    void AudienceTracker::Collect(std::vector<FGuid>& xbox, std::vector<FGuid>& others)
    {
        xbox.clear();
        others.clear();

        std::vector<UObject*> states;
        try
        {
            UObjectGlobals::FindAllOf(STR("PalPlayerState"), states);
        }
        catch (...)
        {
            return;
        }

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
                // Property-only peek (no PE). WarmUnknownPlatforms fills cache over ticks.
                platform = NormalizePlatformUserId(ReadAccountNameProp(state));
                if (!platform.empty())
                {
                    std::lock_guard lock(m_mutex);
                    m_platform_by_uid[key] = platform;
                }
            }

            // Only known gdk_ → Xbox path. Unknown stays on formatted nil-UID (Steam/PC).
            // Empty AccountName on Steam must NOT be treated as Xbox (v1.82 regression).
            if (IsXboxPlatform(platform))
            {
                xbox.push_back(uid);
            }
            else
            {
                others.push_back(uid);
            }
        }
    }

    bool AudienceTracker::IsOnlinePlayerUid(const FGuid& player_uid) const
    {
        if (player_uid == FGuid{})
        {
            return false;
        }

        std::vector<UObject*> states;
        try
        {
            UObjectGlobals::FindAllOf(STR("PalPlayerState"), states);
        }
        catch (...)
        {
            return false;
        }

        for (UObject* state : states)
        {
            FGuid uid{};
            if (ReadPlayerUId(state, uid) && uid == player_uid)
            {
                return true;
            }
        }
        return false;
    }
}
