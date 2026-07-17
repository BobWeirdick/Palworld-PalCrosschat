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
    class WordFilter
    {
    public:
        explicit WordFilter(const Config& config);

        bool Active() const;

        // True if player is currently under an auto-mute.
        bool IsMuted(std::string_view player_key);

        // Returns the matched pattern source if message hits the blacklist.
        std::optional<std::string> FindMatch(std::string_view message) const;

        // Apply AutoMuteMinutes for player_key. No-op if minutes <= 0.
        void Mute(std::string_view player_key);

        int AutoMuteMinutes() const { return m_auto_mute_minutes; }
        const std::string& MuteLogWebhook() const { return m_mute_log_webhook; }
        size_t PatternCount() const { return m_patterns.size(); }

    private:
        struct CompiledPattern
        {
            std::regex re;
            std::string source;
        };

        int m_auto_mute_minutes = 0;
        std::string m_mute_log_webhook;
        std::vector<CompiledPattern> m_patterns;

        mutable std::mutex m_mute_mutex;
        std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_mutes;
    };
}
