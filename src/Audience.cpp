#include "Audience.h"
#include "PlatformId.h"
#include "SehUtil.h"

#include <cstdio>
#include <string>
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
        constexpr const TCHAR* kRequestJoinPath =
            STR("/Script/Pal.PalPlayerState:RequestJoinPlayer_ToServer");
        constexpr int kJoinMaxWaitFrames = 180;
        constexpr int kJoinSoftReadyFrames = 30;

        std::string FormatGuidKey(const FGuid& guid)
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

        bool IsXboxPlatform(const std::string& platform_user_id)
        {
            return platform_user_id.rfind("gdk_", 0) == 0;
        }

        struct ReadIdCtx
        {
            UObject* state = nullptr;
            FGuid uid{};
            bool have_uid = false;
            char platform[256]{};
            bool ok = false;
        };

        void ReadJoinIdentityImpl(ReadIdCtx* ctx)
        {
            ctx->ok = false;
            ctx->have_uid = false;
            ctx->platform[0] = 0;
            if (!ctx->state)
            {
                return;
            }

            if (FProperty* prop = ctx->state->GetPropertyByNameInChain(STR("PlayerUId")))
            {
                if (FGuid* guid = prop->ContainerPtrToValuePtr<FGuid>(ctx->state))
                {
                    ctx->uid = *guid;
                    ctx->have_uid = !(ctx->uid == FGuid{});
                }
            }

            if (FProperty* prop = ctx->state->GetPropertyByNameInChain(STR("AccountName")))
            {
                if (FString* name = prop->ContainerPtrToValuePtr<FString>(ctx->state))
                {
                    const TCHAR* data = **name;
                    if (data)
                    {
                        const std::string utf8 = RC::to_utf8_string(data);
                        const std::string norm = NormalizePlatformUserId(utf8);
                        if (!norm.empty() && norm.size() < sizeof(ctx->platform))
                        {
                            std::snprintf(ctx->platform, sizeof(ctx->platform), "%s", norm.c_str());
                        }
                    }
                }
            }

            ctx->ok = ctx->have_uid;
        }

        int ReadJoinIdentitySeh(ReadIdCtx* ctx)
        {
            __try
            {
                ReadJoinIdentityImpl(ctx);
                return 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                ctx->ok = false;
                return 1;
            }
        }
    }

    AudienceTracker::~AudienceTracker()
    {
        Unregister();
    }

    void AudienceTracker::Register()
    {
        if (m_registered)
        {
            return;
        }

        try
        {
            const auto ids = UObjectGlobals::RegisterHook(
                StringType{kRequestJoinPath},
                &AudienceTracker::OnRequestJoinPlayer,
                UnrealScriptFunctionCallable{},
                this);
            m_hook_ids = ids;
            m_registered = (ids.first >= 0 || ids.second >= 0);
            if (m_registered)
            {
                Output::send<LogLevel::Normal>(
                    STR("[PalCrosschat] Audience roster: join hook on {} "
                        "(leave=weak PlayerState; chat Upsert; no FindAllOf)\n"),
                    kRequestJoinPath);
            }
            else
            {
                Output::send<LogLevel::Warning>(
                    STR("[PalCrosschat] Audience roster: join hook unavailable — "
                        "chat Upsert + weak prune only\n"));
            }
        }
        catch (...)
        {
            m_registered = false;
            m_hook_ids = {-1, -1};
            Output::send<LogLevel::Warning>(
                STR("[PalCrosschat] Audience roster: join hook register failed — continuing\n"));
        }
    }

    void AudienceTracker::Unregister()
    {
        if (!m_registered)
        {
            return;
        }
        try
        {
            UObjectGlobals::UnregisterHook(StringType{kRequestJoinPath}, m_hook_ids);
        }
        catch (...)
        {
        }
        m_registered = false;
        m_hook_ids = {-1, -1};
    }

    void AudienceTracker::OnRequestJoinPlayer(UnrealScriptFunctionCallableContext& context,
                                              void* custom_data)
    {
        auto* self = static_cast<AudienceTracker*>(custom_data);
        if (!self)
        {
            return;
        }
        try
        {
            self->EnqueueJoin(context.Context);
        }
        catch (...)
        {
        }
    }

    void AudienceTracker::EnqueueJoin(UObject* player_state)
    {
        if (!player_state)
        {
            return;
        }
        try
        {
            std::lock_guard lock(m_mutex);
            for (auto& pending : m_pending_joins)
            {
                if (SafeWeakGet(pending.player_state) == player_state)
                {
                    return;
                }
            }
            PendingJoin pending{};
            pending.player_state = player_state;
            pending.frames = 0;
            m_pending_joins.push_back(std::move(pending));
        }
        catch (...)
        {
        }
    }

    bool AudienceTracker::TryReadJoinIdentity(UObject* player_state,
                                              FGuid& out_uid,
                                              std::string& out_platform) const
    {
        out_uid = FGuid{};
        out_platform.clear();
        if (!player_state)
        {
            return false;
        }

        ReadIdCtx ctx{};
        ctx.state = player_state;
        if (ReadJoinIdentitySeh(&ctx) != 0 || !ctx.ok)
        {
            return false;
        }
        out_uid = ctx.uid;
        out_platform = ctx.platform;
        return true;
    }

    void AudienceTracker::ProcessPendingJoins()
    {
        std::vector<PendingJoin> pending;
        {
            std::lock_guard lock(m_mutex);
            pending.swap(m_pending_joins);
        }

        std::vector<PendingJoin> still;
        int added = 0;
        for (auto item : pending)
        {
            ++item.frames;
            UObject* state = SafeWeakGet(item.player_state);
            if (!state)
            {
                continue;
            }

            FGuid uid{};
            std::string platform;
            const bool ok = TryReadJoinIdentity(state, uid, platform);
            const bool timed_out = item.frames >= kJoinMaxWaitFrames;
            const bool soft_ready = ok && item.frames >= kJoinSoftReadyFrames;
            const bool hard_ready = ok && timed_out;

            if (soft_ready || hard_ready)
            {
                Upsert(uid, platform, state);
                ++added;
            }
            else if (!timed_out)
            {
                still.push_back(std::move(item));
            }
        }

        if (!still.empty())
        {
            std::lock_guard lock(m_mutex);
            m_pending_joins.insert(m_pending_joins.end(), still.begin(), still.end());
        }

        if (added > 0)
        {
            Output::send<LogLevel::Normal>(
                STR("[PalCrosschat] Audience roster: {} player(s) from join hook\n"), added);
        }
    }

    void AudienceTracker::RebuildListsUnlocked()
    {
        m_xbox.clear();
        m_others.clear();
        m_xbox.reserve(m_roster.size());
        m_others.reserve(m_roster.size());
        for (const auto& [key, e] : m_roster)
        {
            (void)key;
            if (IsXboxPlatform(e.platform))
            {
                m_xbox.push_back(e.uid);
            }
            else
            {
                m_others.push_back(e.uid);
            }
        }
    }

    void AudienceTracker::TryBootstrapOnce()
    {
        if (m_bootstrapped)
        {
            return;
        }
        m_bootstrapped = true;

        struct FindAllCtx
        {
            std::vector<UObject*>* out = nullptr;
            bool ok = false;
        };

        // SEH in a tiny helper — no C++ objects with destructors inside __try.
        struct Seh
        {
            static int Run(FindAllCtx* ctx)
            {
                __try
                {
                    ctx->out->clear();
                    UObjectGlobals::FindAllOf(STR("PalPlayerState"), *ctx->out);
                    ctx->ok = true;
                    return 0;
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    ctx->ok = false;
                    return 1;
                }
            }
        };

        std::vector<UObject*> states;
        FindAllCtx ctx{};
        ctx.out = &states;
        if (Seh::Run(&ctx) != 0 || !ctx.ok)
        {
            Output::send<LogLevel::Warning>(
                STR("[PalCrosschat] Audience bootstrap FindAllOf faulted — "
                    "continuing with join/chat roster only (no retry)\n"));
            return;
        }

        int added = 0;
        for (UObject* state : states)
        {
            if (!state)
            {
                continue;
            }
            FGuid uid{};
            std::string platform;
            if (!TryReadJoinIdentity(state, uid, platform))
            {
                continue;
            }
            Upsert(uid, platform, state);
            ++added;
        }

        Output::send<LogLevel::Normal>(
            STR("[PalCrosschat] Audience bootstrap: seeded {} player(s) "
                "(one-shot FindAllOf; join/chat after this)\n"),
            added);
    }

    size_t AudienceTracker::Size() const
    {
        try
        {
            std::lock_guard lock(m_mutex);
            return m_roster.size();
        }
        catch (...)
        {
            return 0;
        }
    }

    void AudienceTracker::Tick()
    {
        try
        {
            ProcessPendingJoins();

            int removed = 0;
            {
                std::lock_guard lock(m_mutex);
                std::vector<std::string> dead;
                for (auto& [key, e] : m_roster)
                {
                    if (!e.has_weak)
                    {
                        continue;
                    }
                    if (!SafeWeakGet(e.player_state))
                    {
                        dead.push_back(key);
                    }
                }
                for (const auto& key : dead)
                {
                    m_roster.erase(key);
                    ++removed;
                }
                RebuildListsUnlocked();
            }

            if (removed > 0)
            {
                Output::send<LogLevel::Normal>(
                    STR("[PalCrosschat] Audience roster: removed {} stale player(s) "
                        "(PlayerState gone)\n"),
                    removed);
            }
        }
        catch (const std::exception& ex)
        {
            Output::send<LogLevel::Warning>(
                STR("[PalCrosschat] Audience Tick exception (ignored): {}\n"),
                RC::ensure_str(ex.what()));
        }
        catch (...)
        {
            Output::send<LogLevel::Warning>(
                STR("[PalCrosschat] Audience Tick exception (ignored)\n"));
        }
    }

    void AudienceTracker::Upsert(const FGuid& player_uid,
                                 const std::string& platform_user_id,
                                 UObject* player_state)
    {
        if (player_uid == FGuid{})
        {
            return;
        }
        try
        {
            std::string platform = NormalizePlatformUserId(platform_user_id);
            const std::string key = FormatGuidKey(player_uid);

            std::lock_guard lock(m_mutex);
            const bool was_new = !m_roster.contains(key);
            auto& e = m_roster[key];
            e.uid = player_uid;
            if (!platform.empty())
            {
                e.platform = std::move(platform);
            }
            if (player_state)
            {
                e.player_state = player_state;
                e.has_weak = true;
            }
            RebuildListsUnlocked();

            if (was_new)
            {
                Output::send<LogLevel::Normal>(
                    STR("[PalCrosschat] Audience roster: upsert {} platform={}\n"),
                    RC::ensure_str(key),
                    RC::ensure_str(e.platform.empty() ? "?" : e.platform));
            }
        }
        catch (...)
        {
        }
    }

    void AudienceTracker::Remember(const FGuid& player_uid, const std::string& platform_user_id)
    {
        Upsert(player_uid, platform_user_id, nullptr);
    }

    void AudienceTracker::Forget(const FGuid& player_uid)
    {
        if (player_uid == FGuid{})
        {
            return;
        }
        try
        {
            const std::string key = FormatGuidKey(player_uid);
            std::lock_guard lock(m_mutex);
            if (m_roster.erase(key) > 0)
            {
                RebuildListsUnlocked();
                Output::send<LogLevel::Normal>(
                    STR("[PalCrosschat] Audience roster: forgot {}\n"), RC::ensure_str(key));
            }
        }
        catch (...)
        {
        }
    }

    void AudienceTracker::ForgetMany(const std::vector<FGuid>& player_uids)
    {
        if (player_uids.empty())
        {
            return;
        }
        try
        {
            int n = 0;
            std::lock_guard lock(m_mutex);
            for (const auto& uid : player_uids)
            {
                if (uid == FGuid{})
                {
                    continue;
                }
                if (m_roster.erase(FormatGuidKey(uid)) > 0)
                {
                    ++n;
                }
            }
            if (n > 0)
            {
                RebuildListsUnlocked();
                Output::send<LogLevel::Warning>(
                    STR("[PalCrosschat] Audience roster: forgot {} player(s) after "
                        "inject/target failure\n"),
                    n);
            }
        }
        catch (...)
        {
        }
    }

    void AudienceTracker::Snapshot(std::vector<FGuid>& xbox, std::vector<FGuid>& others)
    {
        try
        {
            std::lock_guard lock(m_mutex);
            std::vector<std::string> dead;
            for (auto& [key, e] : m_roster)
            {
                if (!e.has_weak)
                {
                    continue;
                }
                if (!SafeWeakGet(e.player_state))
                {
                    dead.push_back(key);
                }
            }
            for (const auto& key : dead)
            {
                m_roster.erase(key);
            }
            if (!dead.empty())
            {
                RebuildListsUnlocked();
            }
            xbox = m_xbox;
            others = m_others;
        }
        catch (...)
        {
            xbox.clear();
            others.clear();
        }
    }

    bool AudienceTracker::IsOnlinePlayerUid(const FGuid& player_uid) const
    {
        if (player_uid == FGuid{})
        {
            return false;
        }
        try
        {
            const std::string key = FormatGuidKey(player_uid);
            std::lock_guard lock(m_mutex);
            return m_roster.contains(key);
        }
        catch (...)
        {
            return false;
        }
    }
}
