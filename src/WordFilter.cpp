#include "WordFilter.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Helpers/String.hpp>

namespace PalCrosschat
{
    WordFilter::WordFilter(const Config& config)
        : m_mute_log_webhook(config.mute_log_webhook),
          m_initial_mute_notification(config.initial_mute_notification),
          m_active_mute_notification(config.active_mute_notification)
    {
        m_patterns.reserve(config.filter_patterns.size());
        for (const auto& entry : config.filter_patterns)
        {
            if (entry.pattern.empty())
            {
                continue;
            }
            try
            {
                CompiledPattern compiled;
                compiled.re = std::regex(
                    entry.pattern,
                    std::regex::ECMAScript | std::regex::optimize | std::regex::icase);
                compiled.source = entry.pattern;
                compiled.mute_minutes = entry.mute_minutes < 0 ? 0 : entry.mute_minutes;
                compiled.mute_message = entry.mute_message;
                m_patterns.push_back(std::move(compiled));
            }
            catch (const std::regex_error& ex)
            {
                RC::Output::send<RC::LogLevel::Warning>(
                    STR("[PalCrosschat] ChatFilter: invalid regex skipped ({}): {}\n"),
                    RC::ensure_str(ex.what()),
                    RC::ensure_str(entry.pattern));
            }
        }

        if (!config.filter_patterns.empty())
        {
            RC::Output::send<RC::LogLevel::Normal>(
                STR("[PalCrosschat] ChatFilter: {}/{} patterns compiled, webhook={}\n"),
                m_patterns.size(),
                config.filter_patterns.size(),
                m_mute_log_webhook.empty() ? STR("off") : STR("on"));
        }
    }

    bool WordFilter::Active() const
    {
        return !m_patterns.empty();
    }

    WordFilter::MuteEntry* WordFilter::FindActiveMuteLocked(
        std::string_view player_key,
        std::chrono::steady_clock::time_point now)
    {
        if (player_key.empty())
        {
            return nullptr;
        }

        const auto it = m_mutes.find(std::string(player_key));
        if (it == m_mutes.end())
        {
            return nullptr;
        }
        if (now >= it->second.until)
        {
            m_mutes.erase(it);
            return nullptr;
        }
        return &it->second;
    }

    bool WordFilter::IsMuted(std::string_view player_key)
    {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard lock(m_mute_mutex);
        return FindActiveMuteLocked(player_key, now) != nullptr;
    }

    std::optional<int> WordFilter::MuteRemainingSeconds(std::string_view player_key)
    {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard lock(m_mute_mutex);
        MuteEntry* entry = FindActiveMuteLocked(player_key, now);
        if (!entry)
        {
            return std::nullopt;
        }
        const auto secs =
            std::chrono::duration_cast<std::chrono::seconds>(entry->until - now).count();
        return static_cast<int>(secs > 0 ? secs : 0);
    }

    std::string WordFilter::MuteReason(std::string_view player_key)
    {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard lock(m_mute_mutex);
        MuteEntry* entry = FindActiveMuteLocked(player_key, now);
        if (!entry)
        {
            return {};
        }
        return entry->reason;
    }

    std::optional<FilterHit> WordFilter::FindMatch(std::string_view message) const
    {
        if (message.empty() || m_patterns.empty())
        {
            return std::nullopt;
        }

        const std::string text(message);
        for (const auto& pattern : m_patterns)
        {
            try
            {
                if (std::regex_search(text, pattern.re))
                {
                    FilterHit hit;
                    hit.pattern_source = pattern.source;
                    hit.mute_minutes = pattern.mute_minutes;
                    hit.mute_message = pattern.mute_message;
                    return hit;
                }
            }
            catch (...)
            {
            }
        }
        return std::nullopt;
    }

    void WordFilter::Mute(std::string_view player_key, std::string_view reason, int mute_minutes)
    {
        if (player_key.empty() || mute_minutes <= 0)
        {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        MuteEntry entry;
        entry.until = now + std::chrono::minutes(mute_minutes);
        entry.reason = std::string(reason);
        entry.last_notify = now;

        std::lock_guard lock(m_mute_mutex);
        m_mutes[std::string(player_key)] = std::move(entry);
    }

    bool WordFilter::TryConsumeMuteNotify(std::string_view player_key,
                                          std::chrono::seconds min_interval)
    {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard lock(m_mute_mutex);
        MuteEntry* entry = FindActiveMuteLocked(player_key, now);
        if (!entry)
        {
            return false;
        }
        if (entry->last_notify.time_since_epoch().count() != 0 &&
            now - entry->last_notify < min_interval)
        {
            return false;
        }
        entry->last_notify = now;
        return true;
    }
}
