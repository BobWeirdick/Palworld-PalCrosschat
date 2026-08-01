#include "Audience.h"
#include "PlatformId.h"

#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <DynamicOutput/DynamicOutput.hpp>
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
        constexpr auto kRefreshInterval = std::chrono::milliseconds(3000);

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

        struct FindAllCtx
        {
            std::vector<UObject*>* out = nullptr;
            bool ok = false;
        };

        void FindAllPlayerStatesImpl(FindAllCtx* ctx)
        {
            ctx->out->clear();
            UObjectGlobals::FindAllOf(STR("PalPlayerState"), *ctx->out);
            ctx->ok = true;
        }

        int FindAllPlayerStatesSeh(FindAllCtx* ctx)
        {
            __try
            {
                FindAllPlayerStatesImpl(ctx);
                return 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 1;
            }
        }

        bool SafeFindAllPlayerStates(std::vector<UObject*>& out)
        {
            FindAllCtx ctx{};
            ctx.out = &out;
            if (FindAllPlayerStatesSeh(&ctx) != 0 || !ctx.ok)
            {
                out.clear();
                return false;
            }
            return true;
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

    void AudienceTracker::TickRefresh()
    {
        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard lock(m_mutex);
            if (m_have_snapshot && (now - m_last_refresh) < kRefreshInterval)
            {
                return;
            }
        }
        RefreshNow();
    }

    void AudienceTracker::RefreshNow()
    {
        std::vector<UObject*> states;
        if (!SafeFindAllPlayerStates(states))
        {
            Output::send<LogLevel::Warning>(
                STR("[PalCrosschat] Audience FindAllOf faulted; keeping previous snapshot\n"));
            std::lock_guard lock(m_mutex);
            m_last_refresh = std::chrono::steady_clock::now();
            return;
        }

        struct Entry
        {
            FGuid uid{};
            std::string key;
            std::string platform;
            UObject* state = nullptr;
        };
        std::vector<Entry> entries;
        entries.reserve(states.size());

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

            Entry e{};
            e.uid = uid;
            e.key = FormatGuid(uid);
            e.state = state;

            {
                std::lock_guard lock(m_mutex);
                if (auto it = m_platform_by_uid.find(e.key); it != m_platform_by_uid.end())
                {
                    e.platform = it->second;
                }
            }

            // Property-only: AccountName + Remember() from chat hooks. No UniqueNetId
            // ProcessEvent here — that PE path correlated with delayed UE4SS/PalServer AVs.
            if (e.platform.empty())
            {
                e.platform = NormalizePlatformUserId(ReadAccountNameProp(state));
                if (!e.platform.empty())
                {
                    Remember(uid, e.platform);
                }
            }

            entries.push_back(std::move(e));
        }

        std::vector<FGuid> xbox;
        std::vector<FGuid> others;
        std::unordered_set<std::string> online;
        xbox.reserve(entries.size());
        others.reserve(entries.size());
        online.reserve(entries.size());

        for (const auto& e : entries)
        {
            online.insert(e.key);
            if (IsXboxPlatform(e.platform))
            {
                xbox.push_back(e.uid);
            }
            else
            {
                others.push_back(e.uid);
            }
        }

        {
            std::lock_guard lock(m_mutex);
            m_xbox = std::move(xbox);
            m_others = std::move(others);
            m_online_keys = std::move(online);
            m_have_snapshot = true;
            m_last_refresh = std::chrono::steady_clock::now();
        }
    }

    void AudienceTracker::Snapshot(std::vector<FGuid>& xbox, std::vector<FGuid>& others) const
    {
        std::lock_guard lock(m_mutex);
        xbox = m_xbox;
        others = m_others;
    }

    bool AudienceTracker::IsOnlinePlayerUid(const FGuid& player_uid) const
    {
        if (player_uid == FGuid{})
        {
            return false;
        }
        const std::string key = FormatGuid(player_uid);
        std::lock_guard lock(m_mutex);
        return m_online_keys.contains(key);
    }
}
