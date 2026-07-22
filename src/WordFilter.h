#pragma once

#include "Config.h"

#include <chrono>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace PalCrosschat
{
    struct FilterHit
    {
        std::string pattern_source;
        int mute_minutes = 0;
        std::string mute_message;
    };

    class WordFilter
    {
    public:
        explicit WordFilter(const Config& config);

        bool Active() const;

        bool IsMuted(std::string_view player_key);
        std::optional<int> MuteRemainingSeconds(std::string_view player_key);
        std::string MuteReason(std::string_view player_key);

        std::optional<FilterHit> FindMatch(std::string_view message) const;

        // Mute for the given minutes (from the matched pattern). No-op if minutes <= 0.
        void Mute(std::string_view player_key, std::string_view reason, int mute_minutes);

        bool TryConsumeMuteNotify(std::string_view player_key, std::chrono::seconds min_interval);

        const std::string& MuteLogWebhook() const { return m_mute_log_webhook; }
        const std::string& InitialMuteNotification() const { return m_initial_mute_notification; }
        const std::string& ActiveMuteNotification() const { return m_active_mute_notification; }
        size_t PatternCount() const { return m_patterns.size(); }

    private:
        struct CompiledPattern
        {
            std::regex re;
            std::string source;
            int mute_minutes = 0;
            std::string mute_message;
        };

        struct MuteEntry
        {
            std::chrono::steady_clock::time_point until{};
            std::string reason;
            std::chrono::steady_clock::time_point last_notify{};
        };

        MuteEntry* FindActiveMuteLocked(std::string_view player_key,
                                        std::chrono::steady_clock::time_point now);

        std::string m_mute_log_webhook;
        std::string m_initial_mute_notification;
        std::string m_active_mute_notification;
        std::vector<CompiledPattern> m_patterns;

        mutable std::mutex m_mute_mutex;
        std::unordered_map<std::string, MuteEntry> m_mutes;
    };
}
