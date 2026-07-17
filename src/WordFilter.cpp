#include "WordFilter.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Helpers/String.hpp>

namespace PalCrosschat
{
    WordFilter::WordFilter(const Config& config)
        : m_auto_mute_minutes(config.auto_mute_minutes),
          m_mute_log_webhook(config.mute_log_webhook)
    {
        m_patterns.reserve(config.blocked_words.size());
        for (const auto& pattern : config.blocked_words)
        {
            if (pattern.empty())
            {
                continue;
            }
            try
            {
                CompiledPattern compiled;
                compiled.re = std::regex(pattern, std::regex::ECMAScript | std::regex::optimize);
                compiled.source = pattern;
                m_patterns.push_back(std::move(compiled));
            }
            catch (const std::regex_error& ex)
            {
                RC::Output::send<RC::LogLevel::Warning>(
                    STR("[PalCrosschat] WordBlacklist: invalid regex skipped ({}): {}\n"),
                    RC::ensure_str(ex.what()),
                    RC::ensure_str(pattern));
            }
        }

        if (!config.blocked_words.empty())
        {
            RC::Output::send<RC::LogLevel::Normal>(
                STR("[PalCrosschat] WordBlacklist: {}/{} patterns compiled, AutoMuteMinutes={}, webhook={}\n"),
                m_patterns.size(),
                config.blocked_words.size(),
                m_auto_mute_minutes,
                m_mute_log_webhook.empty() ? STR("off") : STR("on"));
        }
    }

    bool WordFilter::Active() const
    {
        return !m_patterns.empty();
    }

    bool WordFilter::IsMuted(std::string_view player_key)
    {
        if (player_key.empty() || m_auto_mute_minutes <= 0)
        {
            return false;
        }

        const std::string key(player_key);
        const auto now = std::chrono::steady_clock::now();

        std::lock_guard lock(m_mute_mutex);
        const auto it = m_mutes.find(key);
        if (it == m_mutes.end())
        {
            return false;
        }
        if (now >= it->second)
        {
            m_mutes.erase(it);
            return false;
        }
        return true;
    }

    std::optional<std::string> WordFilter::FindMatch(std::string_view message) const
    {
        if (message.empty() || m_patterns.empty())
        {
            return std::nullopt;
        }

        for (const auto& pattern : m_patterns)
        {
            try
            {
                if (std::regex_search(message.begin(), message.end(), pattern.re))
                {
                    return pattern.source;
                }
            }
            catch (const std::regex_error&)
            {
                // Ignore per-message regex runtime errors; keep checking others.
            }
        }
        return std::nullopt;
    }

    void WordFilter::Mute(std::string_view player_key)
    {
        if (player_key.empty() || m_auto_mute_minutes <= 0)
        {
            return;
        }

        const auto until =
            std::chrono::steady_clock::now() + std::chrono::minutes(m_auto_mute_minutes);

        std::lock_guard lock(m_mute_mutex);
        m_mutes[std::string(player_key)] = until;
    }
}
